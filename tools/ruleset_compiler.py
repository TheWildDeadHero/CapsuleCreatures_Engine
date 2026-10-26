#!/usr/bin/env python3
"""!Ruleset formulas and pipelines (P-13, P-14, P-18): restricted integer
expressions with interval analysis, a Python evaluator and C++ code
generation.

A ruleset (`capsule.ruleset/2`) gives the arithmetic of a game's battles:

    { "schema": "capsule.ruleset/2",
      "constants": {"CRIT": 384},
      "inputs":    {"level": [1, 100], "base": [1, 255], ...},
      "formulas":  {"exp_gain": {"expr": "...", "range": [1, 16777215]},
                    "damage_base": {"args": ["level", "power", "attack", "defense"],
                                    "expr": "...", "range": [1, 65535]}, ...},
      "stat_formulas": {"stat_hp": {"expr": "...", "range": [1, 999]}, ...},
      "points":    {"power": {"holder": "user", "sources": ["user.passive", "user.item"],
                              "range": [1, 1023]}, ...},
      "pipelines": {"damage": [{"let": "attack", "expr": "stage_scale(offense_stage_id, offense_raw, offense_stage)",
                                "range": [1, "STAT_CAP"]},
                               {"point": "offense", "on": "attack"},
                               {"roll": "r", "span": "16"}, ...], ...} }

**Formulas.** The fixed slots (FORMULA_SLOTS: exp_gain, reward, escape_odds)
are engine functions with fixed inputs. Any other formula is a named formula:
it declares its "args" (names with ranges in "inputs") and pipelines call it
like a function; each call's arguments must lie inside those ranges. The
battle manifest's resources (D-25, tools/battlec/resources.py) name named
formulas too: a move resource's max (base, raises), regen and cost amounts
(value, max). The former pp_max slot (E6c) is refused (RETIRED_SLOTS): it is
a move resource's max formula now.

**Stat formulas** (D-20 C) are named freely (lower-case identifiers); each
stat of the battle manifest names the one that computes it. They all take
the inputs STAT_INPUTS. The battle compiler emits rules::stat(index, ...),
which calls the stat's formula (emit_stat_dispatch()).

**Pipelines** (D-20 D: PIPELINES lists them with their engine inputs and
outputs) are straight-line step lists the battle compiler emits as C++
(rules::damage, self_hit, speed, accuracy, crit, effect_odds, ai_score, recruit_odds; capsule/battle/pipeline.h;
effect_odds, ai_score (G1: an AI profile's pipeline rule) and recruit_odds (G3: one recruit check's odds) are
optional: OPTIONAL_PIPELINES gives a ruleset without them the plain odds, a score of 0 and odds that never
recruit):
  - {"let": var, "expr": text, "range": [lo, hi]}: computes var (range
    optional: the value is clamped to it; bounds may name constants);
  - {"point": name, "on": var}: runs the modifier point on var (the handlers
    of the point's sources, then the point's range clamp; nothing when no
    source has a handler);
  - {"roll": var, "span": text}: var = a draw in [0, span) at exactly this
    position (no draw when span <= 0: var = 0; previews pass a fixed roll;
    span at most DRAW_SPAN, 65536).
Pipeline expressions read the pipeline's inputs, earlier variables,
constants (STAT_CAP and LEVEL_MAX come from the battle rules), named
formulas and the helpers HELPERS (the battle's stages and type chart, D-22
A6 / A7):
  - stage_scale(id, v, stage): v scaled by stage `id` (the inputs
    offense_stage_id, defense_stage_id, speed_stage_id) at `stage` (clamped
    to the stage's range): its table, its formula or x1;
  - staged(v, stage): the same with the table every stat stage shares (an
    error when the stat stages' tables differ); accuracy_staged(v, stage):
    the accuracy stage's;
  - crit_chance(stage) / crit_out_of(stage): the crit stage's odds;
  - type_scale(v): v scaled by the type chart for the move against the
    target, by the chart's combine mode (exponent: pow2(v, type_exp);
    multiply: the combined factor; sequential: each tier's factor in turn);
  - mulq8(a, b) = (a * b) >> 8 and pow2(v, e) = v * 2^e (e capped at +8 /
    -16) on non-negative values.

**Points** name a holder (user, target or self = user) and their sources in
the order they run: user.passive, user.item, user.status, target.passive,
target.item, target.status, weather ("passive", "item" and "status" mean the
holder's). Every handler sees the value clamped to [0, 65535]; the range
clamps the point's result. Point ids follow the declaration order.

Expression language (integers only, P-6):
  - integer literals, constants (upper-case names from "constants"), inputs
    and variables, parentheses;
  - + - * / % and unary -; / and % truncate toward zero, as in C++;
  - comparisons < <= > >= == != (1 or 0), && and || (1 or 0), ! (1 or 0);
  - bit operators (E13a) on int32 two's complement values: & | ^, unary ~
    (= -v - 1), << and >> (>> is arithmetic: it rounds toward minus
    infinity, as C++20 defines it); a shift amount must lie in
    [0, SHIFT_MAX] (31) and a << result inside int32 (both checked by the
    interval analysis);
  - functions min(a, b), max(a, b), clamp(v, lo, hi), abs(v), isqrt(v)
    (floor square root), isqrt_ceil(v), if(c, a, b).

Precedence (Parser.LEVELS), loosest first: || ; && ; == != ; < <= > >= ; | ;
^ ; & ; << >> ; + - ; * / % ; then the unary - ! ~. The bit operators bind
tighter than comparisons (as in Rust and Python, not C): `a & MASK == 0`
reads as `(a & MASK) == 0`. Every binary level is left-associative.
Generated C++ parenthesizes every operation, so it follows the tree, not
C's precedence.

Names may carry one dot (`species.gender_ratio`): a caller that declares such
inputs (battlec/progression.py: the species fields in derived expressions)
reads them like any other input.

Build-time checks (P-18): every intermediate value of every expression is
bounded by interval analysis over the declared input ranges; the build fails
if any can leave int32, if a divisor's range contains 0, if isqrt can see a
negative argument, or if a shift amount's range leaves [0, SHIFT_MAX]. The
bit operators are bounded soundly for signed values too: two operands whose
values fit k bits plus a sign give a result that does (& of a non-negative
operand stays within it). The generated function clamps its result to the declared
output range; the report says whether the clamp can ever engage. Pipelines
are analysed against the battle's values (bind()): engine inputs, stage
tables, caps.

Used as a module by battle_compiler.py; run directly to check a ruleset (the
pipelines against neutral battle values, DEFAULT_BATTLE):

    ruleset_compiler.py <ruleset.json> [<formula> name=value ...]
"""
from __future__ import annotations

import json
import re
import sys
from collections.abc import Callable, Iterable, Mapping, Sequence
from typing import Any

## The ruleset schema.
SCHEMA = "capsule.ruleset/2"
## Formula slots the engine calls, with their input names in call order
## (capsule/battle/ruleset.h declares the same functions).
FORMULA_SLOTS = {
   "exp_gain": ["yield", "level", "victor_level", "participants", "foe_rank", "outsider"],
   "reward": ["reward_base", "level"],
   "escape_odds": ["speed", "foe_speed", "attempts"],
}
## Formula slots that are gone, with what replaced them (refused with this migration message).
RETIRED_SLOTS = {
   "pp_max": ("the pp_max slot is gone (D-25): a move resource's \"max\" names a formula of (base, raises); declare "
              "it as a named formula, e.g. \"pp_max\": {\"args\": [\"pp_base\", \"pp_ups\"], \"expr\": ..., "
              "\"range\": ...} (tools/battle_resources.py migrates a battle)"),
}
## Inputs of every stat formula, in call order (capsule::rules::stat() passes the stat index first).
STAT_INPUTS = ["base", "gene", "training", "awaken", "level", "trait_pct"]
## A stat formula's, named formula's, point's or variable's name: a lower-case identifier (part of C++ names).
STAT_FORMULA_NAME = re.compile(r"[a-z][a-z0-9_]*")
## A constant's name.
CONSTANT_NAME = re.compile(r"[A-Z][A-Z0-9_]*")
## A ruleset input's name: an identifier (no dot; E13a dotted names are a caller's own inputs).
INPUT_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
## Prefix of a stat formula's generated C++ function.
STAT_FUNCTION_PREFIX = "stat_formula_"
## Prefix of a named formula's generated C++ function.
FORMULA_FUNCTION_PREFIX = "formula_"
## Prefix of a pipeline variable in the generated C++.
VARIABLE_PREFIX = "l_"
## Inputs a growth-rate expression may use (tables built at compile time).
GROWTH_INPUTS = ["n"]
## int32 bounds every intermediate must stay inside.
INT32 = (-(1 << 31), (1 << 31) - 1)
## Largest shift amount of << and >> (int32: a shift by 32 or more, or by a negative amount, is undefined in C++).
SHIFT_MAX = 31
## The bit operators (E13a): bitwise and, or, exclusive or (unary ~ is the tree's "inv" node).
BIT_OPS = ("&", "|", "^")
## The shift operators (E13a).
SHIFT_OPS = ("<<", ">>")
## The unary operators and the tree node each makes (Parser.unary(); E13a: ~ is "inv"; X1: the studio's list).
UNARY_OPERATORS = {"-": "neg", "!": "not", "~": "inv"}
## Functions and their argument counts.
FUNCTIONS = {"min": 2, "max": 2, "clamp": 3, "abs": 1, "isqrt": 1, "isqrt_ceil": 1, "if": 3}
## Pipeline helpers and their argument counts (capsule/battle/pipeline.h, capsule/battle/ruleset.h).
HELPERS = {"staged": 2, "accuracy_staged": 2, "stage_scale": 3, "crit_chance": 1, "crit_out_of": 1, "mulq8": 2,
           "pow2": 2, "type_scale": 1}
## C++ of each helper.
HELPER_CPP = {"staged": "battle::pipeline_staged", "accuracy_staged": "battle::pipeline_accuracy_staged",
              "stage_scale": "battle::pipeline_stage_scale", "crit_chance": "battle::pipeline_crit_chance",
              "crit_out_of": "battle::pipeline_crit_out_of", "mulq8": "rules::mulq8", "pow2": "rules::pow2",
              "type_scale": "battle::pipeline_type_scale"}
## Helpers whose C++ takes the pipeline's context first (they read the battle: the move, the target).
CONTEXT_HELPERS = {"type_scale"}
## Constants the battle rules give pipelines (and the battle values bind() takes them from).
BATTLE_CONSTANTS = {"STAT_CAP": "stat_cap", "LEVEL_MAX": "level_max"}
## Numbers in a range ([lo, hi]).
RANGE_LEN = 2
## Below 2**52 an integer converts to a float exactly, so a float square root is a close first guess.
FLOAT_EXACT_LIMIT = 1 << 52
## Exponent of a square root.
SQUARE_ROOT_POWER = 0.5
## Divisor that halves a value (a square root has half the bits).
HALF = 2
## First level a growth table computes (index 0 is unused; level 1 starts with no EXP).
FIRST_GROWN_LEVEL = 2
## Command-line argument: the ruleset file.
ARG_RULESET = 1
## Command-line argument: the formula to evaluate (its input values follow).
ARG_SLOT = 2
## Exit status of a command line that is not understood.
EXIT_USAGE = 2

# -- engine facts the pipelines' inputs and helpers follow -----------------------------------------------------------

## A stage's range lies within [-STAGE_LIMIT, STAGE_LIMIT] (capsule::battle::kStageLimit).
STAGE_LIMIT = 15
## "No stage" (capsule::battle::kNone8): a stage id input of a stat without a stage.
NO_STAGE = 0xFF
## Types per battler (kMaxTypesPer).
MAX_TYPES_PER = 3
## Q8 factor of x1 (a type chart tier, the neutral factor).
Q8_ONE = 256
## Combine modes of the type chart's scaling tiers (capsule::battle::Combine order).
COMBINE_MODES = ["exponent", "multiply", "sequential"]
## Inputs a run_pipeline() caller may leave out, with the value the engine passes for "none".
INPUT_DEFAULTS = {"offense_stage_id": NO_STAGE, "defense_stage_id": NO_STAGE, "speed_stage_id": NO_STAGE}
## pow2(): most doublings.
POW2_MAX_UP = 8
## pow2(): most halvings.
POW2_MAX_DOWN = 16
## Bits a Q8 factor is scaled by (mulq8()).
Q8_SHIFT = 8
## Highest 16-bit value (point handlers see the value clamped to [0, U16_MAX]; stats, odds).
U16_MAX = 0xFFFF
## A draw's bits (rng::below() scales a 16-bit draw).
DRAW_BITS = 16
## Values a roll may span (rng::below() scales a 16-bit draw to any span up to 65536; G3: a recruit check's odds
## are n in 65536).
DRAW_SPAN = 1 << DRAW_BITS
## Range of a 0/1 value.
BOOL_RANGE = (0, 1)
## Range of a 16-bit signed value (an AI score the ai_score pipeline sees and adds; G1).
INT16_RANGE = (-0x8000, 0x7FFF)
## Highest percent (a move's accuracy).
PERCENT_MAX = 100
## Highest byte.
BYTE_MAX = 0xFF
## Range a point handler sees its value in.
POINT_VALUE_RANGE = (0, U16_MAX)
## Most points (capsule::battle::kMaxPoints).
MAX_POINTS = 16
## Most sources of one point (kMaxPointSources).
MAX_POINT_SOURCES = 8
## Point sources in capsule::battle::PointSource order (E13b: the move's own handlers, each side condition on a
## battler's side and each volatile it has).
POINT_SOURCES = ["user.passive", "user.item", "user.status", "target.passive", "target.item", "target.status",
                 "weather", "move", "user.side", "target.side", "user.volatile", "target.volatile"]
## Sources a point may name without a battler (they mean the holder's).
HOLDER_SOURCES = ["passive", "item", "status", "side", "volatile"]
## The point the engine runs on a move's type before the chart and same-type checks of its hits (E13b;
## capsule::battle::Rules::move_type_point): a ruleset that declares a point of this name lets handlers change a
## move's type (a value that names no type keeps the move's own).
MOVE_TYPE_POINT = "move_type"
## Point holders (self is the user: pipelines of one battler).
HOLDERS = ["user", "target", "self"]

## B1: the combination pipelines' value input (the method's expression, clamped into it by the engine) and the
## highest fee.
COMBINE_VALUE_MAX = (1 << 24) - 1
## B1: the most combination methods (capsule::combine::kMaxMethods; the pipelines' method input).
MAX_COMBINE_METHODS = 16
## B1: the most records one combination takes on any platform (build_settings max_combine_inputs).
MAX_COMBINE_COUNT = 8
## B1: the fewest records one combination takes.
PAIR_COUNT = 2
## B2: the most nursery slots on any platform (build_settings max_nursery_slots; capsule::combine::kMaxNurserySlots).
MAX_NURSERY_SLOTS = 8
## Pipelines the engine runs: their inputs (fields of the inputs struct, capsule/battle/pipeline.h) and outputs
## with the range the engine stores them in. A pipeline with several outputs returns battle::ChanceResult
## (result, chance, out_of: the first output is the result).
PIPELINES = {
   "damage": {"inputs": ["level", "move_power", "offense_raw", "offense_stage", "defense_raw", "defense_stage",
                         "crit", "spread", "same_type", "type_exp", "offense_stage_id", "defense_stage_id"],
              "outputs": {"damage": (0, U16_MAX)}, "struct": "DamageInputs"},
   "self_hit": {"inputs": ["level", "move_power", "offense_raw", "offense_stage", "defense_raw", "defense_stage",
                           "offense_stage_id", "defense_stage_id"],
                "outputs": {"damage": (0, U16_MAX)}, "struct": "SelfHitInputs"},
   "speed": {"inputs": ["speed_raw", "speed_stage", "speed_stage_id"], "outputs": {"speed": (1, U16_MAX)},
             "struct": "SpeedInputs"},
   "accuracy": {"inputs": ["move_accuracy", "accuracy_stage", "evasion_stage"],
                "outputs": {"hit": BOOL_RANGE, "chance": (0, U16_MAX), "out_of": (0, U16_MAX)},
                "struct": "AccuracyInputs"},
   "crit": {"inputs": ["crit_stage", "move_crit_stage"],
            "outputs": {"crit": BOOL_RANGE, "chance": (0, U16_MAX), "out_of": (0, U16_MAX)}, "struct": "CritInputs"},
   # E9: the odds of a secondary effect or a status (effect logic odds()); optional (OPTIONAL_PIPELINES)
   "effect_odds": {"inputs": ["effect_chance", "effect_out_of", "user_stat", "target_stat", "user_level",
                              "target_level"],
                   "outputs": {"hit": BOOL_RANGE, "chance": (0, U16_MAX), "out_of": (0, U16_MAX)},
                   "struct": "EffectOddsInputs"},
   # G1: what an AI profile's pipeline rule adds to a move's score (capsule/battle/data/ai.h); optional
   "ai_score": {"inputs": ["so_far", "damage", "ko", "damaging", "move_accuracy", "move_statuses", "user_hp",
                           "user_max_hp", "target_hp", "target_max_hp", "turn"],
                "outputs": {"score": INT16_RANGE}, "struct": "AiScoreInputs"},
   # G3 (D-26 section 3): the odds of one recruit check (effect `recruit`; structure recruit.checks draws, each
   # chance in out_of; sure: no draw, it succeeds); optional
   "recruit_odds": {"inputs": ["target_hp", "target_max_hp", "recruit_rate", "bonus", "status_bonus", "target_level",
                               "user_level", "turn"],
                    "outputs": {"sure": BOOL_RANGE, "chance": (0, DRAW_SPAN), "out_of": (1, DRAW_SPAN)},
                    "struct": "RecruitOddsInputs"},
   # F1 (D-27 section 1): an encounter roll's trigger odds and its creature's level, run out of battle on the lead
   # (capsule/battle/encounter_lead.h); optional, and pure arithmetic ("no_points": point steps are refused)
   "encounter_rate": {"inputs": ["rate_n", "rate_m", "rate_scale", "lead_level", "method"],
                      "outputs": {"sure": BOOL_RANGE, "chance": (0, DRAW_SPAN), "out_of": (1, DRAW_SPAN)},
                      "struct": "EncounterRateInputs", "no_points": True},
   "encounter_level": {"inputs": ["min_level", "max_level", "lead_level", "level_bias", "method"],
                       "outputs": {"level": (0, BYTE_MAX)}, "struct": "EncounterLevelInputs", "no_points": True},
   # B1 (D-28 section 1): a combination method's odds (0: the inputs are incompatible; an immediate method allows
   # anything above 0) and its fee, run out of battle over the inputs (value: the method's own expression over
   # them, tools/battlec/combine.py); optional, pure arithmetic
   "combine_odds": {"inputs": ["method", "count", "level_min", "level_max", "level_avg", "same_species", "value"],
                    "outputs": {"odds": (0, U16_MAX)}, "struct": "CombineOddsInputs", "no_points": True},
   "combine_fee": {"inputs": ["method", "count", "level_min", "level_max", "level_avg", "result_level", "value"],
                   "outputs": {"fee": (0, COMBINE_VALUE_MAX)}, "struct": "CombineFeeInputs", "no_points": True},
   # B2 (D-28 section 2): the nursery's experience per step of a held record (its level when it was left there) and
   # what taking one back costs (levels_gained since it was left); optional, pure arithmetic
   "nursery_gain": {"inputs": ["level", "slot"], "outputs": {"gain": (0, U16_MAX)}, "struct": "NurseryGainInputs",
                    "no_points": True},
   "nursery_fee": {"inputs": ["levels_gained", "level", "count"], "outputs": {"fee": (0, COMBINE_VALUE_MAX)},
                   "struct": "NurseryFeeInputs", "no_points": True},
}
## The encounter_rate pipeline's rate_scale input: Q8 (256 = x1) up to this (x16; the engine clamps the hooks'
## product to it, so rate_n * rate_scale stays inside 32 bits).
MAX_RATE_SCALE = 4096
## Highest encounter method index (capsule::encounter::kMaxMethods - 1).
MAX_ENCOUNTER_METHOD = 15
## Pipelines a ruleset may leave out, with the steps it then gets. effect_odds: the plain odds effect_chance in
## effect_out_of, drawn as rng::chance() draws them (no draw when they are certain), so a game without the
## pipeline sees odds() behave like chance().
OPTIONAL_PIPELINES = {
   "effect_odds": [
      {"let": "chance", "expr": "effect_chance"},
      {"let": "out_of", "expr": "effect_out_of"},
      {"roll": "r", "span": "if(chance < out_of, out_of, 0)"},
      {"let": "hit", "expr": "chance >= out_of || r < chance"},
   ],
   # G1: a ruleset without it adds nothing to an AI score
   "ai_score": [{"let": "score", "expr": "0"}],
   # G3: a ruleset without it never recruits (a battle with a recruit section needs one: battlec/recruit.py)
   "recruit_odds": [{"let": "sure", "expr": "0"}, {"let": "chance", "expr": "0"}, {"let": "out_of", "expr": "1"}],
   # F1: the table's odds times the hooks' scale, and a level drawn between the lowest and highest exactly as the
   # tables draw one on their own (rng::between(): one draw of the span). The odds keep the scale's 8 fraction
   # bits while the table's out_of has room for them (rate_m <= 256: rate_n * scale in rate_m * 256, the same draw
   # as rate_n in rate_m at x1); a larger out_of drops them.
   "encounter_rate": [{"let": "sure", "expr": "0"},
                      {"let": "chance", "expr": f"if(rate_m <= {1 << Q8_SHIFT}, rate_n * rate_scale, "
                                                f"rate_n * rate_scale >> {Q8_SHIFT})", "range": [0, DRAW_SPAN]},
                      {"let": "out_of", "expr": f"if(rate_m <= {1 << Q8_SHIFT}, rate_m << {Q8_SHIFT}, rate_m)",
                       "range": [1, DRAW_SPAN]}],
   "encounter_level": [{"roll": "r", "span": "max_level - min_level + 1"}, {"let": "level", "expr": "min_level + r"}],
   # B1: the method's own value as the odds (within 0..65535) and as the fee
   "combine_odds": [{"let": "odds", "expr": f"min(value, {U16_MAX})"}],
   "combine_fee": [{"let": "fee", "expr": "value"}],
   # B2: one experience point per step, and no fee
   "nursery_gain": [{"let": "gain", "expr": "1"}],
   "nursery_fee": [{"let": "fee", "expr": "0"}],
}

## Range of the neutral battle's stages (the engine's: every range a stage may have).
DEFAULT_STAGE_RANGE = (-STAGE_LIMIT, STAGE_LIMIT)
## Range of the neutral battle's crit stage (the engine's, from 0).
DEFAULT_CRIT_RANGE = (0, STAGE_LIMIT)
## Highest stat of the neutral battle (stats are stored in 16 bits).
NEUTRAL_STAT_CAP = U16_MAX
## Highest level of the neutral battle (levels are stored in 8 bits).
NEUTRAL_LEVEL_MAX = 0xFF
## A neutral battle for checking a ruleset on its own (main()): the engine's caps and input ranges (no game's
## numbers, D-22 decision 8), x1 stages (one stat stage shared by every stat, accuracy, evasion, a crit stage that
## never hits) and a x1 type chart.
## "stages" entries: {kind: stat | accuracy | evasion | crit, range: (lo, hi), table: [(num, den)] or None,
## odds: [(n, m)] or None, formula: a named formula or None}; "chart": {combine, factors (Q8 of the scaling
## tiers), exps (their exponents), types (most types per battler)}.
DEFAULT_BATTLE = {
   "stat_cap": NEUTRAL_STAT_CAP, "level_max": NEUTRAL_LEVEL_MAX,
   "stages": [{"kind": "stat", "range": DEFAULT_STAGE_RANGE, "table": None, "odds": None, "formula": None},
              {"kind": "accuracy", "range": DEFAULT_STAGE_RANGE, "table": None, "odds": None, "formula": None},
              {"kind": "evasion", "range": DEFAULT_STAGE_RANGE, "table": None, "odds": None, "formula": None},
              {"kind": "crit", "range": DEFAULT_CRIT_RANGE, "odds": [(0, 1)] * (DEFAULT_CRIT_RANGE[1] + 1),
               "table": None, "formula": None}],
   "stat_stage": 0, "accuracy_stage": 1, "evasion_stage": 2, "crit_stage": 3,
   "chart": {"combine": "exponent", "factors": [Q8_ONE], "exps": [0], "types": MAX_TYPES_PER},
}

## An expression tree node: ("num", v), ("var", name), ("bin", op, a, b), ... (see Parser).
Node = tuple[Any, ...]
## A token: (kind, value).
Token = tuple[str, Any]
## An inclusive integer range (lo, hi).
Range = tuple[int, int]
## What helper and named formula calls need: {"formulas": name -> compiled formula, "battle": battle values}.
Env = Mapping[str, Any]
## A point callback of run_pipeline(): (point name, value) -> the value after the handlers, or None when no
## source has a handler.
PointFn = Callable[[str, int], "int | None"]


class RulesetError(Exception):
   """!A ruleset rule was broken; the message says where and how."""
   pass


# -- parsing ----------------------------------------------------------------------------

## Token pattern: numbers, names (one dot allowed: species.gender_ratio), two-character operators, single characters.
TOKEN = re.compile(r"\s*(?:(\d+)|([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)?)"
                   r"|(<<|>>|<=|>=|==|!=|&&|\|\|)|(.))")
## Single-character operators and punctuation of the language.
SINGLE_OPS = "+-*/%()<>!,&|^~"


def tokenize(text: str, where: str) -> list[Token]:
   """!Splits an expression into tokens.
   @param text   the expression
   @param where  location for error messages
   @return list of (kind, value): kind is "num", "name" or "op"
   @throws RulesetError on a character outside the language
   """
   out = []
   pos = 0
   text = text.rstrip()
   while pos < len(text):
      m = TOKEN.match(text, pos)
      if m is None or m.end() == pos:
         raise RulesetError(f"{where}: cannot read {text[pos:]!r}")
      num, name, op2, op1 = m.groups()
      if num is not None:
         out.append(("num", int(num)))
      elif name is not None:
         out.append(("name", name))
      elif op2 is not None:
         out.append(("op", op2))
      elif op1 is not None:
         if op1 not in SINGLE_OPS:
            raise RulesetError(f"{where}: character {op1!r} is not allowed")
         out.append(("op", op1))
      pos = m.end()
   return out


class Parser:
   """!Recursive-descent parser producing a small tree of tuples:
   ("num", v), ("var", name), ("const", name), ("neg", a), ("not", a),
   ("inv", a) (~a), ("bin", op, a, b), ("call", fn, [args])."""

   ## Binary operator precedence, lowest first (the bit operators above the comparisons, as in Rust; module
   ## docstring).
   LEVELS = [["||"], ["&&"], ["==", "!="], ["<", "<=", ">", ">="], ["|"], ["^"], ["&"], ["<<", ">>"], ["+", "-"],
             ["*", "/", "%"]]

   def __init__(self, tokens: list[Token], names: Iterable[str], constants: Mapping[str, int], where: str,
                functions: Mapping[str, int] | None = None) -> None:
      """!Creates a parser.
      @param tokens     from tokenize()
      @param names      allowed input names
      @param constants  name -> int
      @param where      location for error messages
      @param functions  callable names besides FUNCTIONS (helpers, named formulas) -> argument count
      """
      ## Remaining tokens.
      self.tokens = tokens
      ## Read position.
      self.pos = 0
      ## Allowed inputs.
      self.names = set(names)
      ## Ruleset constants.
      self.constants = constants
      ## Location for errors.
      self.where = where
      ## Callable names -> argument counts.
      self.functions = {**FUNCTIONS, **(functions or {})}

   def peek(self) -> Token | None:
      """!The next token, or None at the end.
      @return (kind, value) or None
      """
      return self.tokens[self.pos] if self.pos < len(self.tokens) else None

   def take(self, op: str | None = None) -> Token:
      """!Consumes a token.
      @param op  required operator, or None for any token
      @return the token
      """
      t = self.peek()
      if t is None or (op is not None and t != ("op", op)):
         raise RulesetError(f"{self.where}: expected {op or 'a value'}" + (f", got {t[1]!r}" if t else " at the end"))
      self.pos += 1
      return t

   def parse(self) -> Node:
      """!Parses the whole expression.
      @return tree
      """
      node = self.binary(0)
      if self.peek() is not None:
         raise RulesetError(f"{self.where}: unexpected {self.peek()[1]!r}")
      return node

   def binary(self, level: int) -> Node:
      """!Parses one precedence level (left-associative).
      @param level  index into LEVELS
      @return tree
      """
      if level == len(self.LEVELS):
         return self.unary()
      node = self.binary(level + 1)
      while True:
         t = self.peek()
         if t is None or t[0] != "op" or t[1] not in self.LEVELS[level]:
            return node
         self.pos += 1
         node = ("bin", t[1], node, self.binary(level + 1))

   def unary(self) -> Node:
      """!Parses -x, !x, ~x and atoms.
      @return tree
      """
      t = self.peek()
      if t is not None and t[0] == "op" and t[1] in UNARY_OPERATORS:
         self.pos += 1
         return (UNARY_OPERATORS[t[1]], self.unary())
      return self.atom()

   def atom(self) -> Node:
      """!Parses a number, name, call or parenthesized expression.
      @return tree
      """
      t = self.take()
      if t[0] == "num":
         return ("num", t[1])
      if t == ("op", "("):
         node = self.binary(0)
         self.take(")")
         return node
      if t[0] != "name":
         raise RulesetError(f"{self.where}: unexpected {t[1]!r}")
      name = t[1]
      if self.peek() == ("op", "("):
         functions = self.functions
         if name not in functions:
            raise RulesetError(f"{self.where}: unknown function {name!r}")
         self.pos += 1
         args = [self.binary(0)]
         while self.peek() == ("op", ","):
            self.pos += 1
            args.append(self.binary(0))
         self.take(")")
         if len(args) != functions[name]:
            raise RulesetError(f"{self.where}: {name}() takes {functions[name]} argument(s)")
         return ("call", name, args)
      if name in self.names:
         return ("var", name)
      if name in self.constants:
         return ("const", name)
      raise RulesetError(f"{self.where}: unknown name {name!r} (inputs: {', '.join(sorted(self.names))})")


def parse(text: Any, names: Iterable[str], constants: Mapping[str, int], where: str,
          functions: Mapping[str, int] | None = None) -> Node:
   """!Parses an expression.
   @param text       the expression
   @param names      allowed input names
   @param constants  name -> int
   @param where      location for error messages
   @param functions  callable names besides FUNCTIONS -> argument count
   @return tree
   """
   if not isinstance(text, str) or not text.strip():
      raise RulesetError(f"{where}: expression must be a non-empty string")
   return Parser(tokenize(text, where), names, constants, where, functions).parse()


# -- integer semantics (C++) ----------------------------------------------------------------

def cdiv(a: int, b: int) -> int:
   """!C++ integer division (truncates toward zero).
   @param a  dividend
   @param b  divisor (not 0)
   @return quotient
   """
   q = abs(a) // abs(b)
   return q if (a >= 0) == (b >= 0) else -q


def cmod(a: int, b: int) -> int:
   """!C++ remainder (sign of the dividend).
   @param a  dividend
   @param b  divisor (not 0)
   @return remainder
   """
   return a - cdiv(a, b) * b


def isqrt(v: int) -> int:
   """!Floor square root.
   @param v  value >= 0
   @return floor(sqrt(v))
   """
   if v < 0:
      raise RulesetError("isqrt of a negative value")

   # a first guess (float root, or a power of two above the root), then corrected to the exact floor
   x = int(v ** SQUARE_ROOT_POWER) if v < FLOAT_EXACT_LIMIT else 1 << ((v.bit_length() + 1) // HALF)
   while x * x > v:
      x -= 1
   while (x + 1) * (x + 1) <= v:
      x += 1
   return x


def isqrt_ceil(v: int) -> int:
   """!Ceiling square root.
   @param v  value >= 0
   @return ceil(sqrt(v))
   """
   r = isqrt(v)
   return r if r * r == v else r + 1


def clamp_stage(st: Mapping[str, Any], stage: int) -> int:
   """!A stage value clamped to a stage's range (as the engine does).
   @param st     a battle values stage ({range, ...})
   @param stage  the value
   @return the clamped value
   """
   lo, hi = st["range"]
   return max(lo, min(hi, stage))


def apply_ratio(v: int, ratio: tuple[int, int]) -> int:
   """!v scaled by a stage multiplier (battle::pipeline_stage_scale(): a 0 denominator leaves v).
   @param v      value
   @param ratio  (num, den)
   @return v * num / den, truncated
   """
   num, den = ratio
   return cdiv(v * num, den) if den != 0 else v


def stage_of_battle(battle: Mapping[str, Any], key: str, fn: str) -> int:
   """!The stage a helper reads (battle values "stat_stage", "accuracy_stage" or "crit_stage").
   @param battle  battle values
   @param key     which
   @param fn      the helper (messages)
   @return its index into battle["stages"]
   @throws RulesetError when the battle has none
   """
   index = battle.get(key)
   if index is None:
      why = {"stat_stage": "the table every stat stage shares, but the stat stages' tables differ or there are "
                           "none (stage_scale(offense_stage_id, v, stage) names the stat's own stage)",
             "accuracy_stage": "the accuracy stage, but the battle has none (a stage of kind accuracy)",
             "crit_stage": "the crit stage's odds, but the battle has no crit stage with an odds table"}[key]
      raise RulesetError(f"{fn}() scales by {why}")
   return index


def stage_value(env: Env, index: int, v: int, stage: int) -> int:
   """!v scaled by a stage at a stage value (battle::pipeline_stage_scale()).
   @param env    helpers' battle values and named formulas
   @param index  stage index, or any other value for x1
   @param v      value
   @param stage  stage value
   @return the scaled value
   """
   stages = env["battle"]["stages"]
   if not 0 <= index < len(stages):
      return v
   st = stages[index]
   at = clamp_stage(st, stage)
   if st.get("table") is not None:
      return apply_ratio(v, st["table"][at - st["range"][0]])
   if st.get("formula") is not None:
      f = env["formulas"][st["formula"]]
      r = evaluate(f["tree"], dict(zip(f["inputs"], (v, at))), f.get("constants", {}))
      return max(f["range"][0], min(f["range"][1], r))
   return v


def crit_odds(battle: Mapping[str, Any], stage: int, fn: str) -> tuple[int, int]:
   """!The crit stage's odds at a crit stage value (clamped to its range, as the engine does).
   @param battle  battle values
   @param stage   crit stage value
   @param fn      the helper (messages)
   @return (n, m)
   """
   st = battle["stages"][stage_of_battle(battle, "crit_stage", fn)]
   return st["odds"][clamp_stage(st, stage) - st["range"][0]]


def pow2(v: int, e: int) -> int:
   """!v * 2^e with the exponent capped (rules::pow2(), the engine's type exponent): at most POW2_MAX_UP doublings
   and POW2_MAX_DOWN halvings (truncating).
   @param v  value >= 0
   @param e  exponent
   @return the value
   """
   if e >= 0:
      return v << min(e, POW2_MAX_UP)
   return v >> min(-e, POW2_MAX_DOWN)


def type_scale(env: Env, values: Mapping[str, int], v: int) -> int:
   """!v scaled by the type chart (battle::pipeline_type_scale()): the run's defending tiers (run_pipeline types=,
   the Q8 factors of the target's scaling tiers in type order) by the chart's combine mode; without them the
   exponent mode uses the type_exp input. A pipeline without type_exp (no move, no target) leaves v.
   @param env     helpers' battle values, named formulas and the run's "types"
   @param values  the pipeline's values so far
   @param v       value
   @return the scaled value
   @throws RulesetError when the mode needs the tiers and the run gave none
   """
   combine = env["battle"]["chart"]["combine"]
   types = env.get("types")
   if "type_exp" not in values:
      return v
   if types is None:
      if combine != "exponent":
         raise RulesetError(f"type_scale() with combine {combine!r} needs the target's tiers (run_pipeline types=)")
      return pow2(v, values["type_exp"])
   if combine == "exponent":
      return pow2(v, values["type_exp"])
   if combine == "sequential":
      for q in types:
         v = (v * q) >> Q8_SHIFT
      return v
   q8 = Q8_ONE
   for q in types:
      q8 = min(U16_MAX, (q8 * q) >> Q8_SHIFT)
   return (v * q8) >> Q8_SHIFT


def call_helper(fn: str, v: Sequence[int], env: Env, values: Mapping[str, int]) -> int:
   """!Evaluates a pipeline helper (HELPERS).
   @param fn      helper name
   @param v       argument values
   @param env     helpers' battle values (stages, chart), named formulas and the run's "types"
   @param values  the pipeline's values so far (type_scale() reads type_exp)
   @return int
   """
   battle = env["battle"]
   if fn == "stage_scale":
      return stage_value(env, *v)
   if fn == "staged":
      return stage_value(env, stage_of_battle(battle, "stat_stage", fn), v[0], v[1])
   if fn == "accuracy_staged":
      return stage_value(env, stage_of_battle(battle, "accuracy_stage", fn), v[0], v[1])
   if fn == "crit_chance":
      return crit_odds(battle, v[0], fn)[0]
   if fn == "crit_out_of":
      return crit_odds(battle, v[0], fn)[1]
   if fn == "type_scale":
      return type_scale(env, values, v[0])
   if fn == "mulq8":
      return (v[0] * v[1]) >> Q8_SHIFT
   return pow2(v[0], v[1])


def evaluate(node: Node, values: Mapping[str, int], constants: Mapping[str, int], env: Env | None = None) -> int:
   """!Evaluates a tree with C++ integer semantics.
   @param node       tree from parse()
   @param values     input name -> int
   @param constants  name -> int
   @param env        helpers' battle values and named formulas (pipelines), or None
   @return int
   """
   def ev(n: Node) -> int:
      """!Evaluates a subtree."""
      return evaluate(n, values, constants, env)

   k = node[0]
   if k == "num":
      return node[1]
   if k == "var":
      return values[node[1]]
   if k == "const":
      return constants[node[1]]
   if k == "neg":
      return -ev(node[1])
   if k == "not":
      return int(ev(node[1]) == 0)
   if k == "inv":
      # Python's ~ on its unbounded ints is int32 two's complement's for every int32 value
      return ~ev(node[1])
   if k == "bin":
      _, op, left, right = node
      a = ev(left)

      # && and || short-circuit, as in C++
      if op == "&&":
         return int(a != 0 and ev(right) != 0)
      if op == "||":
         return int(a != 0 or ev(right) != 0)
      b = ev(right)
      if op in BIT_OPS:
         # Python's & | ^ treat negative ints as infinite two's complement: the int32 results for int32 operands
         return {"&": a & b, "|": a | b, "^": a ^ b}[op]
      if op in SHIFT_OPS:
         if not 0 <= b <= SHIFT_MAX:
            raise RulesetError(f"shift amount {b} outside [0, {SHIFT_MAX}]")
         # >> on a negative int rounds toward minus infinity in Python and in C++20 alike
         return a << b if op == "<<" else a >> b
      if op == "+":
         return a + b
      if op == "-":
         return a - b
      if op == "*":
         return a * b
      if op == "/":
         if b == 0:
            raise RulesetError("division by zero")
         return cdiv(a, b)
      if op == "%":
         if b == 0:
            raise RulesetError("remainder by zero")
         return cmod(a, b)
      return int({"<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b, "==": a == b, "!=": a != b}[op])
   _, fn, args = node
   if fn == "if":
      # only the branch taken is evaluated
      cond, yes, no = args
      return ev(yes if ev(cond) != 0 else no)
   v = [ev(a) for a in args]
   if fn == "min":
      return min(v)
   if fn == "max":
      return max(v)
   if fn == "clamp":
      value, lo, hi = v
      return max(lo, min(hi, value))
   if fn == "abs":
      return abs(v[0])
   if fn == "isqrt":
      return isqrt(v[0])
   if fn == "isqrt_ceil":
      return isqrt_ceil(v[0])
   if fn in HELPERS:
      return call_helper(fn, v, env, values)

   # a named formula: its expression over the arguments, clamped to its range
   f = env["formulas"][fn]
   r = evaluate(f["tree"], dict(zip(f["inputs"], v)), constants)
   return max(f["range"][0], min(f["range"][1], r))


# -- interval analysis --------------------------------------------------------------------------

def stage_interval(env: Env, ids: Range, v: Range, stage: Range, fn: str, where: str) -> Range:
   """!Bounds stage_value() over a range of stage ids (ids outside the stages mean x1), values and stage values.
   @param env    helpers' battle values and named formulas
   @param ids    stage id range
   @param v      value range
   @param stage  stage value range
   @param fn     the helper (messages)
   @param where  location for error messages
   @return (lo, hi)
   @throws RulesetError when a product can leave int32 or a formula stage's arguments leave its declared ranges
   """
   stages = env["battle"]["stages"]
   chosen = [i for i in range(max(ids[0], 0), min(ids[1], len(stages) - 1) + 1)]
   out: list[Range] = []
   if ids[0] < 0 or ids[1] >= len(stages) or not chosen:
      out.append(v)   # an id of no stage: x1
   for i in chosen:
      st = stages[i]
      lo_stage, hi_stage = clamp_stage(st, stage[0]), clamp_stage(st, stage[1])
      if st.get("table") is not None:
         ratios = [st["table"][x - st["range"][0]] for x in range(lo_stage, hi_stage + 1)]
         for x in v:
            for num, _den in ratios:
               if not INT32[0] <= x * num <= INT32[1]:
                  raise RulesetError(f"{where}: {fn}() product can leave int32")
         vals = [apply_ratio(x, q) for x in v for q in ratios]
         out.append((min(vals), max(vals)))
      elif st.get("formula") is not None:
         out.append(formula_call_interval(st["formula"], [v, (lo_stage, hi_stage)], env,
                                          f"{where}: {fn}() (stage {i}'s formula)"))
      else:
         out.append(v)
   return (min(r[0] for r in out), max(r[1] for r in out))


def type_scale_interval(chart: Mapping[str, Any], v: Range, where: str) -> Range:
   """!Bounds type_scale() over a value range: up to chart["types"] scaling tiers (fewer when a type is absent:
   x1), each one of chart["factors"], combined by chart["combine"].
   @param chart  battle values chart {combine, factors, exps, types}
   @param v      value range (>= 0)
   @param where  location for error messages
   @return (lo, hi)
   @throws RulesetError when a value can leave int32
   """
   factors = list(chart["factors"]) + [Q8_ONE]
   k = chart["types"]
   if chart["combine"] == "exponent":
      exps = list(chart["exps"]) + [0]
      e_lo, e_hi = k * min(min(exps), 0), k * max(max(exps), 0)
      if pow2(v[1], e_hi) > INT32[1]:
         raise RulesetError(f"{where}: type_scale() can leave int32")
      return (pow2(v[0], e_lo), pow2(v[1], e_hi))
   lo_q, hi_q = min(factors), max(factors)
   if chart["combine"] == "sequential":
      lo, hi = v
      for _ in range(k):
         if hi * hi_q > INT32[1]:
            raise RulesetError(f"{where}: type_scale() can leave int32")
         lo, hi = (lo * lo_q) >> Q8_SHIFT, (hi * hi_q) >> Q8_SHIFT
      return (min(lo, v[0]), max(hi, v[1]))
   q_lo, q_hi = Q8_ONE, Q8_ONE
   for _ in range(k):
      q_lo, q_hi = min(U16_MAX, (q_lo * lo_q) >> Q8_SHIFT), min(U16_MAX, (q_hi * hi_q) >> Q8_SHIFT)
   q_lo, q_hi = min(q_lo, Q8_ONE), max(q_hi, Q8_ONE)
   if v[1] * q_hi > INT32[1]:
      raise RulesetError(f"{where}: type_scale() can leave int32")
   return ((v[0] * q_lo) >> Q8_SHIFT, (v[1] * q_hi) >> Q8_SHIFT)


def helper_interval(fn: str, r: Sequence[Range], env: Env, where: str) -> Range:
   """!Bounds a helper call over its argument ranges.
   @param fn     helper name
   @param r      argument ranges
   @param env    helpers' battle values (stages, chart) and named formulas
   @param where  location for error messages
   @return (lo, hi) (int32 is checked by the caller)
   @throws RulesetError if a non-negative argument can be negative, or the battle lacks what the helper reads
   """
   battle = env["battle"]
   try:
      if fn == "stage_scale":
         return stage_interval(env, *r, fn, where)
      if fn in ("staged", "accuracy_staged"):
         index = stage_of_battle(battle, "stat_stage" if fn == "staged" else "accuracy_stage", fn)
         return stage_interval(env, (index, index), r[0], r[1], fn, where)
      if fn in ("crit_chance", "crit_out_of"):
         part = 0 if fn == "crit_chance" else 1
         st = battle["stages"][stage_of_battle(battle, "crit_stage", fn)]
         span = range(clamp_stage(st, r[0][0]), clamp_stage(st, r[0][1]) + 1)
         odds = [crit_odds(battle, x, fn)[part] for x in span]
         return (min(odds), max(odds))
   except RulesetError as e:
      # stage_of_battle() does not know the location
      if str(e).startswith(where):
         raise
      raise RulesetError(f"{where}: {e}")
   for i, a in enumerate(r):
      if a[0] < 0 and not (fn == "pow2" and i == 1):
         raise RulesetError(f"{where}: {fn}() argument {i + 1} can be negative ([{a[0]}, {a[1]}])")
   if fn == "type_scale":
      return type_scale_interval(battle["chart"], r[0], where)
   if fn == "mulq8":
      if r[0][1] * r[1][1] > INT32[1]:
         raise RulesetError(f"{where}: mulq8() product can leave int32")
      return ((r[0][0] * r[1][0]) >> Q8_SHIFT, (r[0][1] * r[1][1]) >> Q8_SHIFT)
   if pow2(r[0][1], r[1][1]) > INT32[1]:
      raise RulesetError(f"{where}: pow2() can leave int32")
   return (pow2(r[0][0], r[1][0]), pow2(r[0][1], r[1][1]))


def formula_call_interval(fn: str, r: Sequence[Range], env: Env, where: str) -> Range:
   """!Bounds a named formula call: each argument must lie inside the formula's declared input range.
   @param fn     formula name
   @param r      argument ranges
   @param env    named formulas
   @param where  location for error messages
   @return the formula's result range (its bounds over these arguments, clamped to its declared range)
   @throws RulesetError if an argument can leave its declared range
   """
   f = env["formulas"][fn]
   for name, have in zip(f["inputs"], r):
      want = f["ranges"][name]
      if have[0] < want[0] or have[1] > want[1]:
         raise RulesetError(f"{where}: {fn}() argument {name} can reach [{have[0]}, {have[1]}], outside its "
                            f"declared range [{want[0]}, {want[1]}]")
   b = interval(f["tree"], dict(zip(f["inputs"], r)), f["constants"], where, [])
   lo, hi = f["range"]
   return (max(lo, min(hi, b[0])), max(lo, min(hi, b[1])))


def magnitude_bits(r: Range) -> int:
   """!The bits k such that every value of a range lies in [-2^k, 2^k - 1] (two's complement with k bits and a sign).
   @param r  (lo, hi)
   @return k
   """
   # a negative v needs the bits of -v - 1 (its complement), a non-negative one its own
   return max((v if v >= 0 else -v - 1).bit_length() for v in r)


def bit_interval(op: str, a: Range, b: Range) -> Range:
   """!Bounds a & b, a | b or a ^ b over two ranges (sound for signed values: two's complement).
   @param op  "&", "|" or "^"
   @param a   left operand's range
   @param b   right operand's range
   @return (lo, hi)
   """
   if a[0] >= 0 and b[0] >= 0:
      # non-negative operands: no bit above the wider one's highest; & clears bits, | only sets them
      top = (1 << max(a[1], b[1]).bit_length()) - 1
      if op == "&":
         return (0, min(a[1], b[1]))
      if op == "|":
         return (max(a[0], b[0]), top)
      return (0, top)
   if op == "&" and (a[0] >= 0 or b[0] >= 0):
      # & with a non-negative operand keeps only that operand's bits
      hi = a[1] if a[0] >= 0 else b[1]
      return (0, hi)

   # in general: both operands fit k bits and a sign, and so does every bitwise result of them
   k = max(magnitude_bits(a), magnitude_bits(b))
   return (-(1 << k), (1 << k) - 1)


def shift_interval(op: str, a: Range, b: Range, where: str) -> Range:
   """!Bounds a << b or a >> b (b must stay within [0, SHIFT_MAX]).
   @param op     "<<" or ">>"
   @param a      shifted value's range
   @param b      shift amount's range
   @param where  location for error messages
   @return (lo, hi)
   @throws RulesetError when the amount's range leaves [0, SHIFT_MAX]
   """
   if b[0] < 0 or b[1] > SHIFT_MAX:
      raise RulesetError(f"{where}: shift amount range [{b[0]}, {b[1]}] must lie within [0, {SHIFT_MAX}]")

   # a << s = a * 2^s and a >> s = floor(a / 2^s) are monotonic in a for a fixed s and in s for a fixed a (up or
   # down by a's sign), so the extremes are at the four corners
   corners = [x << s if op == "<<" else x >> s for x in a for s in b]
   return (min(corners), max(corners))


def interval(node: Node, ranges: Mapping[str, Range], constants: Mapping[str, int], where: str,
             notes: list[tuple[str, Range]], env: Env | None = None) -> Range:
   """!Bounds a tree over input ranges and checks every intermediate (P-18).
   @param node       tree from parse()
   @param ranges     input name -> (lo, hi)
   @param constants  name -> int
   @param where      location for error messages
   @param notes      list collecting the checked nodes (for reports)
   @param env        helpers' battle values and named formulas (pipelines), or None
   @return (lo, hi)
   @throws RulesetError if a value can leave int32, a divisor range holds 0,
           isqrt can see a negative value or a shift amount can leave [0, SHIFT_MAX]
   """
   def check(r: Range, what: str) -> Range:
      """!Records an intermediate's range after checking that it stays inside int32."""
      if r[0] < INT32[0] or r[1] > INT32[1]:
         raise RulesetError(f"{where}: {what} can reach [{r[0]}, {r[1]}], outside int32")
      notes.append((what, r))
      return r

   def sub(n: Node) -> Range:
      """!Bounds a subtree."""
      return interval(n, ranges, constants, where, notes, env)

   k = node[0]
   if k == "num":
      return check((node[1], node[1]), str(node[1]))
   if k == "var":
      return ranges[node[1]]
   if k == "const":
      c = constants[node[1]]
      return check((c, c), node[1])
   if k == "neg":
      a = sub(node[1])
      return check((-a[1], -a[0]), "negation")
   if k == "not":
      sub(node[1])
      return (0, 1)
   if k == "inv":
      # ~v = -v - 1: decreasing, so the bounds swap
      a = sub(node[1])
      return check((-a[1] - 1, -a[0] - 1), "bitwise not")
   if k == "bin":
      _, op, left, right = node
      a = sub(left)
      b = sub(right)
      if op in BIT_OPS:
         return check(bit_interval(op, a, b), f"bitwise {op}")
      if op in SHIFT_OPS:
         return check(shift_interval(op, a, b, where), "shift")
      if op == "+":
         return check((a[0] + b[0], a[1] + b[1]), "sum")
      if op == "-":
         return check((a[0] - b[1], a[1] - b[0]), "difference")
      if op == "*":
         # with signs, the extremes are among the four corner products
         p = [a[0] * b[0], a[0] * b[1], a[1] * b[0], a[1] * b[1]]
         return check((min(p), max(p)), "product")
      if op in "/%":
         if b[0] <= 0 <= b[1]:
            raise RulesetError(f"{where}: divisor range [{b[0]}, {b[1]}] contains 0")
         if op == "/":
            q = [cdiv(x, y) for x in a for y in b]
            return check((min(q), max(q)), "quotient")
         # |remainder| < |divisor|, and the remainder has the dividend's sign
         m = max(abs(b[0]), abs(b[1])) - 1
         lo = 0 if a[0] >= 0 else -min(m, abs(a[0]))
         hi = 0 if a[1] <= 0 else min(m, a[1])
         return check((lo, hi), "remainder")
      return (0, 1)
   _, fn, args = node
   r = [sub(x) for x in args]
   if fn == "if":
      # either branch can be taken
      _cond, (yes_lo, yes_hi), (no_lo, no_hi) = r
      return (min(yes_lo, no_lo), max(yes_hi, no_hi))
   if fn == "min":
      return (min(r[0][0], r[1][0]), min(r[0][1], r[1][1]))
   if fn == "max":
      return (max(r[0][0], r[1][0]), max(r[0][1], r[1][1]))
   if fn == "clamp":
      # clamping is monotonic in each argument: clamp the lows together and the highs together
      (v_lo, v_hi), (lo_lo, lo_hi), (hi_lo, hi_hi) = r
      return (max(lo_lo, min(hi_lo, v_lo)), max(lo_hi, min(hi_hi, v_hi)))
   if fn == "abs":
      a = r[0]
      if a[0] >= 0:
         return a
      if a[1] <= 0:
         return check((-a[1], -a[0]), "abs")
      return check((0, max(-a[0], a[1])), "abs")
   if fn in HELPERS:
      return check(helper_interval(fn, r, env, where), f"{fn}()")
   if fn not in ("isqrt", "isqrt_ceil"):
      return formula_call_interval(fn, r, env, where)
   if r[0][0] < 0:
      raise RulesetError(f"{where}: {fn}() argument can be negative ([{r[0][0]}, {r[0][1]}])")
   f = isqrt if fn == "isqrt" else isqrt_ceil
   return (f(r[0][0]), f(r[0][1]))


# -- C++ emission -------------------------------------------------------------------------------

def to_cpp(node: Node, constants: Mapping[str, int], names: Mapping[str, str] | None = None) -> str:
   """!C++ source for a tree (int32_t arithmetic; same semantics as evaluate()).
   @param node       tree from parse()
   @param constants  name -> int (inlined)
   @param names      variable name -> C++ expression (default: the name itself)
   @return expression text
   """
   def cpp(n: Node) -> str:
      """!C++ of a subtree."""
      return to_cpp(n, constants, names)

   k = node[0]
   if k == "num":
      return f"int32_t{{{node[1]}}}"
   if k == "var":
      return names.get(node[1], node[1]) if names else node[1]
   if k == "const":
      return f"int32_t{{{constants[node[1]]}}}"
   if k == "neg":
      return f"(-{cpp(node[1])})"
   if k == "not":
      return f"int32_t({cpp(node[1])} == 0)"
   if k == "inv":
      return f"(~{cpp(node[1])})"
   if k == "bin":
      _, op, left, right = node
      a, b = cpp(left), cpp(right)
      if op in ("&&", "||"):
         return f"int32_t(({a} != 0) {op} ({b} != 0))"
      if op in ("<", "<=", ">", ">=", "==", "!="):
         return f"int32_t({a} {op} {b})"
      return f"({a} {op} {b})"
   _, fn, sub = node
   args = [cpp(x) for x in sub]
   if fn == "if":
      cond, yes, no = args
      return f"(({cond}) != 0 ? ({yes}) : ({no}))"
   if fn == "clamp":
      value, lo, hi = args
      return f"rules::clamp32({value}, {lo}, {hi})"
   if fn in ("min", "max"):
      return f"rules::{fn}32({args[0]}, {args[1]})"
   if fn == "abs":
      return f"rules::abs32({args[0]})"
   if fn in ("isqrt", "isqrt_ceil"):
      return f"rules::{fn}({args[0]})"
   if fn in CONTEXT_HELPERS:
      return f"{HELPER_CPP[fn]}(ctx, {', '.join(args)})"
   if fn in HELPERS:
      return f"{HELPER_CPP[fn]}({', '.join(args)})"
   return f"{FORMULA_FUNCTION_PREFIX}{fn}({', '.join(args)})"


# -- rulesets -----------------------------------------------------------------------------------

def check_range(rng: Any, constants: Mapping[str, int], where: str, ordered: bool = True) -> Range:
   """!A [lo, hi] range whose bounds are ints or constant names.
   @param rng        the JSON value
   @param constants  name -> int
   @param where      location for error messages
   @param ordered    check lo <= hi (False while the battle constants are still placeholders, load())
   @return (lo, hi)
   @throws RulesetError when it is not a range inside int32
   """
   if not (isinstance(rng, list) and len(rng) == RANGE_LEN):
      raise RulesetError(f"{where}: range must be [lo, hi]")
   out = []
   for v in rng:
      if isinstance(v, str) and v in constants:
         v = constants[v]
      if not isinstance(v, int) or isinstance(v, bool):
         raise RulesetError(f"{where}: range bounds are integers or constant names ({v!r})")
      out.append(v)
   if ordered and out[0] > out[1]:
      raise RulesetError(f"{where}: range must be [lo, hi] with lo <= hi")
   if out[0] < INT32[0] or out[1] > INT32[1]:
      raise RulesetError(f"{where}: range outside int32")
   return (out[0], out[1])


def compile_formula(name: str, spec: Any, inputs: Sequence[str], constants: Mapping[str, int],
                    ranges: Mapping[str, Range], where: str) -> dict[str, Any]:
   """!Parses and checks one formula.
   @param name       slot or formula name
   @param spec       {"expr": text, "range": [lo, hi]} (a named formula also has "args")
   @param inputs     the formula's input names
   @param constants  name -> int
   @param ranges     input name -> (lo, hi)
   @param where      location for error messages
   @return dict with tree, range (declared), bounds (analysed), clamp_active, inputs and their ranges, constants
   """
   if not isinstance(spec, dict) or set(spec) - {"expr", "range", "args"} or "expr" not in spec or "range" not in spec:
      raise RulesetError(f"{where}: a formula is {{\"expr\": text, \"range\": [lo, hi]}}")
   rng = check_range(spec["range"], {}, where)
   for n in inputs:
      if n not in ranges:
         raise RulesetError(f"{where}: input {n!r} needs a range in \"inputs\"")
   tree = parse(spec["expr"], inputs, constants, where)
   notes = []
   bounds = interval(tree, ranges, constants, where, notes)
   return {"name": name, "inputs": list(inputs), "tree": tree, "expr": spec["expr"], "range": rng,
           "bounds": bounds, "clamp_active": bounds[0] < rng[0] or bounds[1] > rng[1],
           "ranges": {n: ranges[n] for n in inputs}, "constants": constants}


def resolve_source(source: Any, holder: str, where: str) -> str:
   """!A point source as POINT_SOURCES names it.
   @param source  the JSON value ("user.passive", "passive" = the holder's, "weather", ...)
   @param holder  the point's holder (user, target, self)
   @param where   location for error messages
   @return the POINT_SOURCES name
   @throws RulesetError for an unknown source
   """
   if isinstance(source, str) and source in HOLDER_SOURCES:
      source = f"{'target' if holder == 'target' else 'user'}.{source}"
   if source not in POINT_SOURCES:
      raise RulesetError(f"{where}: unknown source {source!r} (sources: {', '.join(POINT_SOURCES)}; "
                         f"{', '.join(HOLDER_SOURCES)} mean the holder's)")
   return source


def compile_points(specs: Any, constants: Mapping[str, int], where: str) -> dict[str, dict[str, Any]]:
   """!The points section (ids in declaration order).
   @param specs      {name: {"holder", "sources", "range"}}
   @param constants  constants, with the battle constants (their values may be placeholders until bind())
   @param where      location for error messages
   @return name -> {"id", "holder", "sources" (POINT_SOURCES names), "range_spec"}
   """
   if not isinstance(specs, dict):
      raise RulesetError(f"{where}: points must be an object {{name: point}}")
   if len(specs) > MAX_POINTS:
      raise RulesetError(f"{where}: at most {MAX_POINTS} points")
   out = {}
   for name, p in specs.items():
      w = f"{where}.{name}"
      if not STAT_FORMULA_NAME.fullmatch(name):
         raise RulesetError(f"{w}: a point name is a lower-case identifier")
      if not isinstance(p, dict) or set(p) - {"holder", "sources", "range", "combine"} or "range" not in p:
         raise RulesetError(f"{w}: a point is {{\"holder\", \"sources\": [...], \"range\": [lo, hi]}}")
      if "combine" in p:
         raise RulesetError(f"{w}: \"combine\" is reserved (points combine sequentially)")
      holder = p.get("holder", "user")
      if holder not in HOLDERS:
         raise RulesetError(f"{w}: holder must be one of {', '.join(HOLDERS)}")
      sources = p.get("sources", [])
      if not isinstance(sources, list) or len(sources) > MAX_POINT_SOURCES:
         raise RulesetError(f"{w}: sources must be a list of at most {MAX_POINT_SOURCES}")
      resolved = [resolve_source(s, holder, f"{w}.sources") for s in sources]
      if len(set(resolved)) != len(resolved):
         raise RulesetError(f"{w}: a source is listed twice")
      check_range(p["range"], constants, f"{w}.range", ordered=False)
      out[name] = {"id": len(out), "holder": holder, "sources": resolved, "range_spec": p["range"]}
   return out


def compile_pipelines(specs: Any, constants: Mapping[str, int], points: Mapping[str, Any],
                      functions: Mapping[str, int], where: str) -> dict[str, dict[str, Any]]:
   """!The pipelines section: parses every step (analysis needs the battle: bind()).
   @param specs      {pipeline: [step, ...]} for every name in PIPELINES
   @param constants  constants, with the battle constants
   @param points     compile_points() result
   @param functions  callable names besides FUNCTIONS (helpers, named formulas) -> argument count
   @param where      location for error messages
   @return pipeline -> {"steps": [{"kind", "var", "tree", "expr", "range_spec", "point"}], "inputs", "outputs"}
   """
   if not isinstance(specs, dict):
      raise RulesetError(f"{where}: pipelines must be an object {{name: [steps]}}")
   unknown = set(specs) - set(PIPELINES)
   if unknown:
      raise RulesetError(f"{where}: unknown pipeline(s) {', '.join(sorted(unknown))} (pipelines: "
                         f"{', '.join(PIPELINES)})")
   missing = [p for p in PIPELINES if p not in specs and p not in OPTIONAL_PIPELINES]
   specs = {**{p: steps for p, steps in OPTIONAL_PIPELINES.items() if p not in specs}, **specs}
   if missing:
      raise RulesetError(f"{where}: missing pipeline(s) {', '.join(missing)}")
   out = {}
   for name, contract in PIPELINES.items():
      w = f"{where}.{name}"
      steps = specs[name]
      if not isinstance(steps, list):
         raise RulesetError(f"{w}: a pipeline is a list of steps")
      inputs = contract["inputs"]
      known = list(inputs)
      parsed = []
      for i, s in enumerate(steps):
         ws = f"{w}[{i}]"
         kinds = [k for k in ("let", "point", "roll") if isinstance(s, dict) and k in s]
         if len(kinds) != 1:
            raise RulesetError(f"{ws}: a step is {{\"let\", \"expr\", \"range\"}}, {{\"point\", \"on\"}} or "
                               f"{{\"roll\", \"span\"}}")
         kind = kinds[0]
         allowed = {"let": {"let", "expr", "range"}, "point": {"point", "on"}, "roll": {"roll", "span"}}[kind]
         if set(s) - allowed:
            raise RulesetError(f"{ws}: unknown key(s) {', '.join(sorted(set(s) - allowed))}")
         step: dict[str, Any] = {"kind": kind}
         if kind == "point" and contract.get("no_points"):
            raise RulesetError(f"{ws}: {name} runs out of battle (on the lead): it has no point steps")
         if kind == "point":
            if s["point"] not in points:
               raise RulesetError(f"{ws}: unknown point {s['point']!r}")
            var = s.get("on")
            if var not in known or var in inputs:
               raise RulesetError(f"{ws}: \"on\" names a variable set by an earlier let or roll (not an input)")
            step.update({"point": s["point"], "var": var})
         else:
            var = s[kind]
            if not isinstance(var, str) or not STAT_FORMULA_NAME.fullmatch(var):
               raise RulesetError(f"{ws}: a variable name is a lower-case identifier")
            if var in inputs:
               raise RulesetError(f"{ws}: {var!r} is an input of the pipeline")
            text = s.get("expr" if kind == "let" else "span")
            step.update({"var": var, "expr": text,
                         "tree": parse(text, known, constants, ws, functions)})
            if kind == "let" and "range" in s:
               check_range(s["range"], constants, f"{ws}.range", ordered=False)
               step["range_spec"] = s["range"]
            if var not in known:
               known.append(var)
         parsed.append(step)
      for o in contract["outputs"]:
         if o not in known or o in inputs:
            raise RulesetError(f"{w}: needs a step that sets the output {o!r}")
      out[name] = {"steps": parsed, "inputs": list(inputs), "outputs": list(contract["outputs"])}
   return out


def load(data: Any, where: str = "ruleset") -> dict[str, Any]:
   """!Checks a capsule.ruleset/2 manifest and compiles every formula; parses the points and pipelines (bind()
   analyses them against a battle).
   @param data   parsed JSON (editor keys already stripped)
   @param where  location for error messages
   @return {"constants", "inputs", "formulas": name -> compiled, "stat_formulas", "points", "pipelines"}
   @throws RulesetError
   """
   if not isinstance(data, dict):
      raise RulesetError(f"{where}: must be an object")
   unknown = set(data) - {"schema", "id", "constants", "inputs", "formulas", "stat_formulas", "points", "pipelines"}
   if unknown:
      raise RulesetError(f"{where}: unknown key(s) {', '.join(sorted(unknown))}")
   if data.get("schema") != SCHEMA:
      raise RulesetError(f"{where}: schema must be {SCHEMA}")
   constants = data.get("constants", {})
   for k, v in constants.items():
      if not CONSTANT_NAME.fullmatch(k):
         raise RulesetError(f"{where}: constant names are upper case ({k!r})")
      if k in BATTLE_CONSTANTS:
         raise RulesetError(f"{where}: constant {k} comes from the battle rules")
      if not isinstance(v, int) or isinstance(v, bool) or not INT32[0] <= v <= INT32[1]:
         raise RulesetError(f"{where}: constant {k} must be an int32")
   ranges = {}
   for k, v in (data.get("inputs") or {}).items():
      if not INPUT_NAME.fullmatch(k):
         # a dotted name reads as one token now (species.NAME); the ruleset's own inputs are C++ identifiers
         raise RulesetError(f"{where}: input {k!r}: must be an identifier (letters, digits, _)")
      if not (isinstance(v, list) and len(v) == RANGE_LEN and all(isinstance(x, int) for x in v) and v[0] <= v[1]):
         raise RulesetError(f"{where}: input {k}: range must be [lo, hi]")
      ranges[k] = (v[0], v[1])
   formulas = data.get("formulas") or {}
   for slot, why in RETIRED_SLOTS.items():
      if slot in formulas and not (isinstance(formulas[slot], dict) and "args" in formulas[slot]):
         raise RulesetError(f"{where}: {why}")
   unknown = [s for s, f in formulas.items() if s not in FORMULA_SLOTS and not (isinstance(f, dict) and "args" in f)]
   if unknown:
      raise RulesetError(f"{where}: unknown formula slot(s) {', '.join(sorted(unknown))} "
                         f"(slots: {', '.join(FORMULA_SLOTS)}; a named formula declares \"args\")")
   missing = [s for s in FORMULA_SLOTS if s not in formulas]
   if missing:
      raise RulesetError(f"{where}: missing formula(s) {', '.join(missing)}")
   out = {}
   for s in FORMULA_SLOTS:
      out[s] = compile_formula(s, formulas[s], FORMULA_SLOTS[s], constants, ranges, f"{where}: {s}")
   for name, spec in formulas.items():
      if name in FORMULA_SLOTS:
         if "args" in spec:
            raise RulesetError(f"{where}: {name}: a slot's inputs are fixed (no \"args\")")
         continue
      w = f"{where}: {name}"
      if not STAT_FORMULA_NAME.fullmatch(name) or name in FUNCTIONS or name in HELPERS:
         raise RulesetError(f"{w}: a named formula is a lower-case identifier that is not a function name")
      args = spec["args"]
      if not isinstance(args, list) or not all(isinstance(a, str) for a in args) or len(set(args)) != len(args):
         raise RulesetError(f"{w}: args must be a list of distinct input names")
      out[name] = compile_formula(name, spec, args, constants, ranges, w)
   stat_specs = data.get("stat_formulas") or {}
   if not isinstance(stat_specs, dict):
      raise RulesetError(f"{where}: stat_formulas must be an object {{name: formula}}")
   stat_out = {}
   for name, spec in stat_specs.items():
      if not STAT_FORMULA_NAME.fullmatch(name) or name in FORMULA_SLOTS:
         raise RulesetError(f"{where}: stat formula {name!r}: a lower-case identifier that is not a slot name")
      stat_out[name] = compile_formula(name, spec, STAT_INPUTS, constants, ranges, f"{where}: stat_formulas.{name}")

   # points and pipelines see the battle constants too (their values come with bind())
   names = {**constants, **{k: 0 for k in BATTLE_CONSTANTS}}
   points = compile_points(data.get("points") or {}, names, f"{where}: points")
   functions = {**HELPERS, **{n: len(f["inputs"]) for n, f in out.items() if n not in FORMULA_SLOTS}}
   pipelines = compile_pipelines(data.get("pipelines") or {}, names, points, functions, f"{where}: pipelines")
   return {"constants": constants, "inputs": ranges, "formulas": out, "stat_formulas": stat_out,
           "points": points, "pipelines": pipelines}


def named_formulas(compiled: Mapping[str, Any]) -> dict[str, Any]:
   """!The named formulas (callable from pipelines) of a compiled ruleset.
   @param compiled  from load()
   @return name -> compiled formula
   """
   return {n: f for n, f in compiled["formulas"].items() if n not in FORMULA_SLOTS}


def stage_range(battle: Mapping[str, Any], kinds: Sequence[str]) -> Range:
   """!The values the stages of some kinds can take (0 alone when there are none: the engine passes 0).
   @param battle  battle values
   @param kinds   stage kinds
   @return (lo, hi)
   """
   ranges = [st["range"] for st in battle["stages"] if st["kind"] in kinds] or [(0, 0)]
   return (min(r[0] for r in ranges), max(r[1] for r in ranges))


def pipeline_inputs(battle: Mapping[str, Any]) -> dict[str, dict[str, Range]]:
   """!The range of every pipeline input the engine passes (battle values may narrow them: "inputs").
   @param battle  battle values: stat_cap, level_max, stages, chart and optionally "inputs" {pipeline: {input:
                  range}}
   @return pipeline -> input -> (lo, hi)
   """
   stage = stage_range(battle, ["stat"])
   stats = (1, battle["stat_cap"])
   level = (1, battle["level_max"])
   power = (1, U16_MAX >> Q8_SHIFT)
   ids = (0, NO_STAGE)
   chart = battle["chart"]
   exps = list(chart["exps"]) + [0] if chart["combine"] == "exponent" else [0]
   type_exp = (chart["types"] * min(exps), chart["types"] * max(exps))
   crit = stage_range(battle, ["crit"])
   out = {
      "damage": {"level": level, "move_power": power, "offense_raw": stats, "offense_stage": stage,
                 "defense_raw": stats, "defense_stage": stage, "crit": BOOL_RANGE, "spread": BOOL_RANGE,
                 "same_type": BOOL_RANGE, "type_exp": type_exp, "offense_stage_id": ids, "defense_stage_id": ids},
      "self_hit": {"level": level, "move_power": power, "offense_raw": stats, "offense_stage": stage,
                   "defense_raw": stats, "defense_stage": stage, "offense_stage_id": ids, "defense_stage_id": ids},
      "speed": {"speed_raw": stats, "speed_stage": stage, "speed_stage_id": ids},
      "accuracy": {"move_accuracy": (1, U16_MAX >> Q8_SHIFT), "accuracy_stage": stage_range(battle, ["accuracy"]),
                   "evasion_stage": stage_range(battle, ["evasion"])},
      "crit": {"crit_stage": crit, "move_crit_stage": (0, crit[1])},
      "effect_odds": {"effect_chance": (1, U16_MAX), "effect_out_of": (1, U16_MAX), "user_stat": (0, stats[1]),
                      "target_stat": (0, stats[1]), "user_level": level, "target_level": level},
      "ai_score": {"so_far": INT16_RANGE, "damage": (0, U16_MAX), "ko": BOOL_RANGE, "damaging": BOOL_RANGE,
                   "move_accuracy": (0, PERCENT_MAX), "move_statuses": (0, BYTE_MAX), "user_hp": (0, stats[1]),
                   "user_max_hp": stats, "target_hp": (0, stats[1]), "target_max_hp": stats, "turn": (0, U16_MAX)},
      # G3: the battle narrows recruit_rate to its species field's range and status_bonus to its table's
      "recruit_odds": {"target_hp": (0, stats[1]), "target_max_hp": stats, "recruit_rate": (0, BYTE_MAX),
                       "bonus": (0, BYTE_MAX), "status_bonus": (0, BYTE_MAX), "target_level": level,
                       "user_level": level, "turn": (0, U16_MAX)},
      # F1: the encounter pipelines (no lead: lead_level 0)
      "encounter_rate": {"rate_n": (0, U16_MAX), "rate_m": (1, U16_MAX), "rate_scale": (0, MAX_RATE_SCALE),
                         "lead_level": (0, level[1]), "method": (0, MAX_ENCOUNTER_METHOD)},
      "encounter_level": {"min_level": level, "max_level": level, "lead_level": (0, level[1]),
                          "level_bias": (0, BYTE_MAX), "method": (0, MAX_ENCOUNTER_METHOD)},
      # B1: the combination pipelines (levels of the inputs given; value clamped by the engine)
      "combine_odds": {"method": (0, MAX_COMBINE_METHODS - 1), "count": (PAIR_COUNT, MAX_COMBINE_COUNT),
                       "level_min": level, "level_max": level, "level_avg": level, "same_species": BOOL_RANGE,
                       "value": (0, COMBINE_VALUE_MAX)},
      "combine_fee": {"method": (0, MAX_COMBINE_METHODS - 1), "count": (PAIR_COUNT, MAX_COMBINE_COUNT),
                      "level_min": level, "level_max": level, "level_avg": level, "result_level": level,
                      "value": (0, COMBINE_VALUE_MAX)},
      # B2: the nursery pipelines (a held record's level, its slot; the levels it gained, the records held)
      "nursery_gain": {"level": level, "slot": (0, MAX_NURSERY_SLOTS - 1)},
      "nursery_fee": {"levels_gained": (0, level[1]), "level": level, "count": (0, MAX_NURSERY_SLOTS)},
   }
   for name, given in (battle.get("inputs") or {}).items():
      out[name].update({k: tuple(v) for k, v in given.items()})
   return out


def neutral_battle(compiled: Mapping[str, Any]) -> dict[str, Any]:
   """!The neutral battle a ruleset is checked against on its own (DEFAULT_BATTLE), with the highest level its
   own "level" input declares and, as the stat cap, the highest value its stat formulas reach (a ruleset written
   for fewer levels or smaller stats than the engine's caps is checked over what it supports).
   @param compiled  from load()
   @return battle values for bind()
   """
   battle = dict(DEFAULT_BATTLE)
   level = compiled.get("inputs", {}).get("level")
   if level is not None:
      battle["level_max"] = max(1, min(int(level[1]), NEUTRAL_LEVEL_MAX))
   stat_tops = [f["range"][1] for f in compiled.get("stat_formulas", {}).values()]
   if stat_tops:
      battle["stat_cap"] = max(1, min(max(stat_tops), NEUTRAL_STAT_CAP))
   return battle


def bind(compiled: Mapping[str, Any], battle: Mapping[str, Any] | None = None,
         where: str = "ruleset") -> dict[str, Any]:
   """!Analyses the points and pipelines of a ruleset against a battle (P-18): the battle constants, the stage
   tables and the engine's input ranges. Every step's bounds, every point's range and every output's range are
   checked.
   @param compiled  from load()
   @param battle    stat_cap, level_max, stages, stat_stage / accuracy_stage / evasion_stage / crit_stage
                    (indices or None), chart and optionally "inputs" (see DEFAULT_BATTLE and pipeline_inputs());
                    None: neutral_battle()
   @param where     location for error messages
   @return a copy of compiled with "battle", "pipeline_constants", "env", points with "range" and pipelines with
           per-step "bounds" / "clamp_active" and "ranges" (variable -> range)
   @throws RulesetError
   """
   battle = dict(battle or neutral_battle(compiled))
   constants = {**compiled["constants"], **{k: battle[v] for k, v in BATTLE_CONSTANTS.items()}}
   env = {"formulas": named_formulas(compiled), "battle": battle}
   points = {}
   for name, p in compiled["points"].items():
      points[name] = {**p, "range": check_range(p["range_spec"], constants, f"{where}: points.{name}.range")}
   inputs = pipeline_inputs(battle)
   pipelines = {}
   for name, pl in compiled["pipelines"].items():
      w = f"{where}: pipelines.{name}"
      ranges = dict(inputs[name])
      steps = []
      for i, s in enumerate(pl["steps"]):
         ws = f"{w}[{i}]"
         s = dict(s)
         if s["kind"] == "point":
            # untouched when no source has a handler, else clamped to the point's range
            lo, hi = points[s["point"]]["range"]
            have = ranges[s["var"]]
            s["bounds"] = (min(have[0], lo), max(have[1], hi))
         elif s["kind"] == "roll":
            span = interval(s["tree"], ranges, constants, ws, [], env)
            if span[1] > DRAW_SPAN:
               raise RulesetError(f"{ws}: span can reach {span[1]} (at most {DRAW_SPAN})")
            s["bounds"] = (0, max(0, span[1] - 1))
         else:
            b = interval(s["tree"], ranges, constants, ws, [], env)
            s["analysed"] = b
            if "range_spec" in s:
               lo, hi = check_range(s["range_spec"], constants, f"{ws}.range")
               s["range"] = (lo, hi)
               s["clamp_active"] = b[0] < lo or b[1] > hi
               b = (max(lo, min(hi, b[0])), max(lo, min(hi, b[1])))
            s["bounds"] = b
         ranges[s["var"]] = s["bounds"]
         steps.append(s)
      for o, (lo, hi) in PIPELINES[name]["outputs"].items():
         have = ranges[o]
         if have[0] < lo or have[1] > hi:
            raise RulesetError(f"{w}: output {o} can reach [{have[0]}, {have[1]}]; the engine needs [{lo}, {hi}]")
      pipelines[name] = {**pl, "steps": steps, "ranges": ranges}
   return {**compiled, "battle": battle, "pipeline_constants": constants, "env": env, "points": points,
           "pipelines": pipelines}


def formula_of(compiled: Mapping[str, Any], name: str) -> Mapping[str, Any]:
   """!A compiled formula (slot or named) or stat formula by name.
   @param compiled  from load()
   @param name      formula or stat formula name
   @return the compiled formula
   @throws KeyError when there is none of that name
   """
   if name in compiled["formulas"]:
      return compiled["formulas"][name]
   return compiled.get("stat_formulas", {})[name]


def run(compiled: Mapping[str, Any], slot: str, **values: int) -> int:
   """!Evaluates a compiled formula (slot, named or stat formula) as the generated C++ would, clamp included.
   @param compiled  from load()
   @param slot      formula or stat formula name
   @param values    input values
   @return int
   """
   f = formula_of(compiled, slot)
   v = evaluate(f["tree"], values, compiled["constants"])
   return max(f["range"][0], min(f["range"][1], v))


def run_pipeline(bound: Mapping[str, Any], name: str, inputs: Mapping[str, int], point: PointFn | None = None,
                 roll: int | Callable[[int], int] = 0, types: Sequence[int] | None = None) -> dict[str, Any]:
   """!Runs a pipeline as its generated C++ does (rules::damage() and the others; the studio's trace).
   @param bound   from bind()
   @param name    pipeline name (PIPELINES)
   @param inputs  every input of the pipeline
   @param point   point callback: (point, value) -> the value after the sources' handlers (the battle compiler's
                  point_runner() runs them with effect_compiler.evaluate_pure), or None when no source has a
                  handler; the point's range clamp is applied here. None: no handlers anywhere
   @param roll    a fixed roll (index; clamped to the span, as previews pass one) or a callable span -> draw
   @param types   the Q8 factors of the target's scaling tiers in its type order (type_scale(); the exponent
                  combine mode does without them), or None
   @return {"outputs": {output: value}, "vars": {variable: value}, "trace": [(step index, kind, var, value)]}
   """
   pl = bound["pipelines"][name]
   constants = bound["pipeline_constants"]
   # the stage ids may be left out (INPUT_DEFAULTS: no stage)
   values = {k: int(inputs[k]) if k in inputs else INPUT_DEFAULTS[k] for k in pl["inputs"]}
   env = {**bound["env"], "types": None if types is None else list(types)}
   trace = []
   for i, s in enumerate(pl["steps"]):
      var = s["var"]
      if s["kind"] == "point":
         got = point(s["point"], values[var]) if point is not None else None
         if got is not None:
            lo, hi = bound["points"][s["point"]]["range"]
            values[var] = max(lo, min(hi, got))
      elif s["kind"] == "roll":
         span = evaluate(s["tree"], values, constants, env)
         if span <= 0:
            values[var] = 0
         elif callable(roll):
            values[var] = roll(span)
         else:
            values[var] = max(0, min(span - 1, roll))
      else:
         v = evaluate(s["tree"], values, constants, env)
         if "range" in s:
            v = max(s["range"][0], min(s["range"][1], v))
         values[var] = v
      trace.append((i, s["kind"], var, values[var]))
   return {"outputs": {o: values[o] for o in pl["outputs"]}, "vars": values, "trace": trace}


def growth_table(text: Any, constants: Mapping[str, int], levels: int, where: str, lo: int = 0,
                 hi: int = INT32[1]) -> list[int]:
   """!A growth-rate table: an expression in n evaluated for n = 1..levels at
   build time, clamped to [lo, hi] and forced non-decreasing (P-18). The
   value at n = 1 is always 0 (a monster starts at level 1 with no EXP).
   @param text       expression in n
   @param constants  name -> int
   @param levels     highest level
   @param where      location for error messages
   @param lo         lowest value
   @param hi         highest value
   @return list of levels + 1 ints (index 0 unused, 0)
   """
   tree = parse(text, GROWTH_INPUTS, constants, where)
   interval(tree, {"n": (1, levels)}, constants, where, [])
   out = [0, 0]
   for n in range(FIRST_GROWN_LEVEL, levels + 1):
      v = max(lo, min(hi, evaluate(tree, {"n": n}, constants)))
      out.append(max(v, out[-1]))
   return out


def used_inputs(tree: Node) -> set[str]:
   """!The variables a tree reads.
   @param tree  tree from parse()
   @return names
   """
   used = set()

   def walk(n: Node) -> None:
      """!Collects the variables of a subtree."""
      if n[0] == "var":
         used.add(n[1])
      for c in n[1:]:
         if isinstance(c, tuple):
            walk(c)
         elif isinstance(c, list):
            for x in c:
               walk(x)
   walk(tree)
   return used


def emit_functions(compiled: Mapping[str, Any], stat_formulas: Iterable[str] | None = None) -> list[str]:
   """!C++ definitions of the formula slots, named formulas and stat formulas (inside namespace capsule::rules).
   The named formulas (FORMULA_FUNCTION_PREFIX + name, called by the pipelines) and stat formulas
   (STAT_FUNCTION_PREFIX + name, called by emit_stat_dispatch()) are file-local.
   @param compiled       from load()
   @param stat_formulas  names of the stat formulas to emit (the ones the battle's stats use), or None for all
   @return list of lines
   """
   used_stats = list(compiled.get("stat_formulas", {}) if stat_formulas is None else dict.fromkeys(stat_formulas))
   L = []
   functions = [(slot, slot, f, False) for slot, f in compiled["formulas"].items() if slot in FORMULA_SLOTS]
   local = [(FORMULA_FUNCTION_PREFIX + n, n, f, True) for n, f in named_formulas(compiled).items()]
   local += [(STAT_FUNCTION_PREFIX + name, name, compiled["stat_formulas"][name], False) for name in used_stats]
   for index, (function, slot, f, maybe_unused) in enumerate(functions + local):
      if index == len(functions):
         L += ["", "namespace", "{", ""]
      args = ", ".join(f"int32_t {n}" for n in f["inputs"])
      used = used_inputs(f["tree"])
      L.append(f"// {slot} = {f['expr']}")
      L.append(f"//   analysed [{f['bounds'][0]}, {f['bounds'][1]}], declared [{f['range'][0]}, {f['range'][1]}]"
               + (" (clamp can engage)" if f["clamp_active"] else " (clamp never engages)"))
      L.append(("[[maybe_unused]] " if maybe_unused else "") + f"int32_t {function}({args}) {{")
      for n in f["inputs"]:
         if n not in used:
            L.append(f"    (void){n};")
      L.append(f"    return rules::clamp32({to_cpp(f['tree'], compiled['constants'])}, "
               f"int32_t{{{f['range'][0]}}}, int32_t{{{f['range'][1]}}});")
      L.append("}")
   if local:
      L += ["", "} // namespace"]
   return L


def emit_stat_dispatch(stat_formulas: Sequence[str]) -> list[str]:
   """!C++ definition of capsule::rules::stat(): stat index -> the formula of that stat (emit_functions() emits
   the formulas).
   @param stat_formulas  the formula name of each stat, in stat order
   @return list of lines
   """
   args = ", ".join(f"int32_t {n}" for n in STAT_INPUTS)
   call = ", ".join(STAT_INPUTS)
   L = [f"int32_t stat(int32_t index, {args}) {{", "    switch (index) {"]
   for index, name in enumerate(stat_formulas):
      L.append(f"    case {index}: return {STAT_FUNCTION_PREFIX}{name}({call});")
   # an index outside the stats: the first stat's formula (the engine never asks for one)
   L += [f"    default: return {STAT_FUNCTION_PREFIX}{stat_formulas[0]}({call});", "    }", "}"]
   return L


def camel(name: str) -> str:
   """!CamelCase of a snake_case name.
   @param name  e.g. "self_hit"
   @return e.g. "SelfHit"
   """
   return "".join(p[:1].upper() + p[1:] for p in name.split("_"))


def point_constant(name: str) -> str:
   """!The generated C++ constant of a point id.
   @param name  point name
   @return e.g. "kPointPower"
   """
   return f"kPoint{camel(name)}"


def emit_pipelines(bound: Mapping[str, Any]) -> list[str]:
   """!C++ definitions of the pipelines (inside namespace capsule::rules, after emit_functions()): the point ids
   kPoint + Name (kPointPower ...) and rules::damage / self_hit / speed / accuracy / crit as straight-line code (P-14).
   @param bound  from bind()
   @return list of lines
   """
   constants = bound["pipeline_constants"]
   L = ["", "namespace", "{", ""]
   for name, p in bound["points"].items():
      L.append(f"[[maybe_unused]] constexpr uint8_t {point_constant(name)} = {p['id']};   // sources: "
               + (", ".join(p["sources"]) or "none") + f"; range [{p['range'][0]}, {p['range'][1]}]")
   L += ["", "} // namespace"]
   for name, pl in bound["pipelines"].items():
      contract = PIPELINES[name]
      chance = len(contract["outputs"]) > 1
      result = "battle::ChanceResult" if chance else "int32_t"
      L += ["", f"{result} {name}(battle::PipelineCtx& ctx, const battle::{contract['struct']}& in) {{"]
      names = {n: f"in.{n}" for n in pl["inputs"]}
      reads = set()
      for s in pl["steps"]:
         if "tree" in s:
            reads |= used_inputs(s["tree"])
      if not any(s["kind"] in ("point", "roll") for s in pl["steps"]):
         L.append("    (void)ctx;")
      if not any(n in reads for n in pl["inputs"]):
         L.append("    (void)in;")
      declared = set()
      for s in pl["steps"]:
         var = s["var"]
         target = VARIABLE_PREFIX + var
         if s["kind"] == "point":
            L.append(f"    {target} = battle::run_point(ctx, {point_constant(s['point'])}, {target});")
            continue
         expr = to_cpp(s["tree"], constants, names)
         if s["kind"] == "roll":
            L.append(f"    // {var} = roll in [0, {s['expr']})")
            expr = f"battle::pipeline_roll(ctx, {expr})"
         else:
            L.append(f"    // {var} = {s['expr']}   analysed [{s['analysed'][0]}, {s['analysed'][1]}]"
                     + (f", clamped to [{s['range'][0]}, {s['range'][1]}]" if "range" in s else ""))
            if "range" in s:
               expr = f"rules::clamp32({expr}, int32_t{{{s['range'][0]}}}, int32_t{{{s['range'][1]}}})"
         if var in declared:
            L.append(f"    {target} = {expr};")
         else:
            L.append(f"    int32_t {target} = {expr};")
            L.append(f"    (void){target};")
            declared.add(var)
         names[var] = target
      outs = [VARIABLE_PREFIX + o for o in contract["outputs"]]
      L.append(f"    return battle::ChanceResult{{{', '.join(outs)}}};" if chance else f"    return {outs[0]};")
      L.append("}")
   return L


def main(argv: Sequence[str]) -> int:
   """!Checks a ruleset and optionally evaluates one formula; see the module docstring.
   @param argv  sys.argv
   @return process exit status
   """
   if len(argv) <= ARG_RULESET:
      print(__doc__)
      return EXIT_USAGE
   try:
      with open(argv[ARG_RULESET], encoding="utf-8") as fh:
         data = json.load(fh)
      data = {k: v for k, v in data.items() if k != "editor" and not k.startswith("_")}
      compiled = load(data, argv[ARG_RULESET])
      bound = bind(compiled, None, argv[ARG_RULESET])
      if len(argv) > ARG_SLOT:
         values = dict(a.split("=", 1) for a in argv[ARG_SLOT + 1:])
         print(run(compiled, argv[ARG_SLOT], **{k: int(v) for k, v in values.items()}))
         return 0
   except (RulesetError, OSError, json.JSONDecodeError, KeyError, ValueError) as e:
      print(f"error: {e}", file=sys.stderr)
      return 1
   for slot, f in list(compiled["formulas"].items()) + list(compiled["stat_formulas"].items()):
      state = "clamp can engage" if f["clamp_active"] else "always inside"
      print(f"{slot}: [{f['bounds'][0]}, {f['bounds'][1]}] -> [{f['range'][0]}, {f['range'][1]}] ({state})")
   for name, pl in bound["pipelines"].items():
      outs = ", ".join(f"{o} [{pl['ranges'][o][0]}, {pl['ranges'][o][1]}]" for o in pl["outputs"])
      print(f"pipeline {name}: {len(pl['steps'])} steps -> {outs} (neutral battle values)")
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv))
