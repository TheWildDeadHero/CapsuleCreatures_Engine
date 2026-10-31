"""!Field rules (D-27 sections 1 and 2, package F1): structure.json `lead` and `field_effects`.

**The lead** (capsule::battle::lead_of(), include/capsule/battle/encounter_lead.h) is the party member whose passive
and held item run the encounter hooks (`encounter`, `encounter_made`) and whose level the pipelines see. Without a
`lead` section it is the first member that can battle (hit points above 0). `"lead": {"when": EXPR}` makes it the
first member for which EXPR holds: a growth-style `when` expression over the member's record (its roles, stats,
species fields; tools/battlec/growth.py WhenScope, compiled as one more growth rule with the script trigger).
C8: `"hook_when": EXPR` (optional) picks another member for the hooks of a passive and a held item (the first one
EXPR holds for: hook_lead_of()); the lead (`when`) stays the one whose level the pipelines see and the holder of
the game's global rules (a repel-style level check). E.g. `"hook_when": "1"`: the first party slot, fainted or not,
runs its passive and held item, while a repel-style rule compares with the first member that can battle.
C9: `"allow_eggs": true` lets the rules see eggs (default: an egg is skipped; an egg a rule picks still runs no
passive or held item: an egg in the first slot runs none); the default lead never is an egg unless eggs may battle.

**Timed field effects** are records {"name", "label", "unit", "stack", "expiry", "persists"}:
  - name: a lower-case identifier (effect logic, scripts and the ids header name it);
  - label: what menus show (at most MAX_LABEL_CHARS letters; default the name);
  - unit: "steps" (the walker's) or "minutes" (the game's clock, capsule/core/clock.h clock::minutes());
  - stack: what starting it while it runs does: "refresh" (start over; default), "add" (add the amount) or
    "refuse" (nothing: an item used for it is not used up);
  - expiry: the message shown when it ends (field text; absent: none);
  - persists: whether it survives a save (default true).
The script compiler (tools/script_compiler.py) gives each one an implicit permanent u16 variable
(var_name(): what it has left) and, with an expiry message, an implicit script (end_script_name()), and emits the
records with the script bank (capsule/script/data.h FieldEffectInfo); the battle data emits their count and the ids
(FIELD_EFFECT_<NAME>) so effect logic (`field_effect start NAME N`, `field.effect_active(NAME)`) and game code agree.

**Field actions** (D-27 section 3, package F2) are records {"name", "label", "requires", "target", "ask", "prompt",
"script"}:
  - name: a lower-case identifier (scripts' `field_action available NAME`, the ids header FIELD_ACTION_<NAME>);
  - label: what menus show (at most MAX_LABEL_CHARS letters; default the name);
  - requires: an expression over a party member and the game state (absent: any member): the growth `when`
    language over the member's record (roles, stats.NAME, species.NAME) plus `knows.MOVE` (1 when it knows the move;
    the move's lower-case identifier), `passive` (its passive: `passive == passive.NAME`, 0 = none) and the game
    state `flag.NAME` (0 / 1) and `var.NAME` (a script flag or variable of the scripts manifest: a badge, a story
    step). It is compiled as one more growth rule with the script trigger (the first member it holds for uses the
    action); the flags and variables become the game references (game_refs(): the script compiler resolves them);
  - target: {"behavior": NAME} (a world behavior faced; offered when the walker cannot enter it as it is),
    {"object": KIND} (a map object of the world's object_kinds faced), {"tag": TAG} (a menu action on maps with
    that tag of the world's map_tags) or absent / null (a menu action anywhere);
  - ask: true (default) shows prompt with yes / no before the script runs; false runs it at once;
  - prompt: the question (field text; default "Use LABEL?");
  - script: the script it runs (with the member's party slot in the implicit variable MEMBER_VAR).
The battle data emits each requirement's growth rule (kFieldRules.first_action_rule + the action's index) and the
script compiler the records (capsule/script/data.h FieldActionInfo) with the world's targets resolved.

**Step effects** (D-27 section 4, package F3) are records {"name", "label", "every", "damage", "messages",
"script"}: every `every` steps of the walker the effect runs the effect-logic hook `steps` on each party member
(holder = the member: the game's global rules, its status, its passive, its held item; `field.step_effect` reads
which effect runs). What the logic does is the rule (status damage, a friendship-like role gained, an egg-like
counter run down, a field effect shortened); the record says what the engine does around it:
  - name: a lower-case identifier (the logic's `field.step_effect == NAME`, the ids header STEP_EFFECT_<NAME>);
  - label: what menus and HUDs show (at most MAX_LABEL_CHARS letters; default the name);
  - every: steps between two runs (1 to MAX_EVERY);
  - damage: what a member whose hit points the run took to 0 does: "faint" (default: it stays at 0) or "survive"
    (it keeps 1 hit point);
  - messages: {moment: message key} for STEP_MOMENTS (hurt: the run took hit points, survived: the survive rule
    kept it at 1, fainted: it reached 0): the battle's messages ({user} is the member), queued in the field
    context as a `say` is; a moment left out shows nothing;
  - allow_eggs (C9): whether the run's handlers also run on eggs (default false: an egg is skipped; the egg cycle
    still counts eggs down);
  - script: a game script run after a run in which a member was hurt, survived or fainted (absent: none), with the
    implicit variables MEMBER_STEP_VAR (C8: the party slot of the first member, in party order, that had the run's
    worst outcome; capsule/battle/step_effects.h) and OUTCOME_STEP_VAR (the worst outcome: STEP_OUTCOMES index).
The script compiler gives each one an implicit permanent u16 variable (step_var_name(): the steps counted towards
its next run, range 0 to every - 1: the save image's Script section keeps it under the save shape and remap rules)
and emits the records with the script bank (capsule/script/data.h StepEffectInfo, the messages' ids resolved); the
battle data emits their count (kFieldRules.step_effect_count) and the ids.
"""
from __future__ import annotations

import re
from collections.abc import Mapping, Sequence
from typing import Any

from .common import NONE8, NONE16, Json, ManifestError, check_keys, ident, register_keys
from .growth import WhenScope, compile_rule

## Key of the lead rule in structure.json.
LEAD_KEY = "lead"
## Key of the timed field effects in structure.json.
FIELD_EFFECTS_KEY = "field_effects"
## Units in capsule::script::FieldEffectUnit order.
UNITS = ["steps", "minutes"]
## Stack rules in capsule::script::FieldEffectStack order.
STACKS = ["refresh", "add", "refuse"]
## The stack rule of an effect that names none.
DEFAULT_STACK = "refresh"
## Most field effects (capsule::script::kMaxFieldEffects: a pending-expiry bit each).
MAX_FIELD_EFFECTS = 32
## Longest label (letters).
MAX_LABEL_CHARS = 16
## Longest expiry message (letters).
MAX_EXPIRY_CHARS = 200
## A field effect's name.
NAME = re.compile(r"[a-z][a-z0-9_]*\Z")
## Prefix of an effect's implicit script variable (what it has left).
VAR_PREFIX = "field_effect_"
## Prefix of an effect's implicit expiry script.
END_SCRIPT_PREFIX = "field_effect_end_"
## Type of the implicit variable (steps or minutes, 0 = not running).
VAR_TYPE = "u16"
## Highest amount an effect holds (its u16 variable).
MAX_AMOUNT = 0xFFFF
## capsule::script::kFieldEffectPersists.
PERSISTS_FLAG = 1
## C8: the lead section's optional rule for the encounter hooks' member (its passive and held item run them).
HOOK_WHEN_KEY = "hook_when"
## The trigger a lead rule is compiled for (growth TRIGGERS: a rule that no event triggers).
LEAD_TRIGGER = "script"
## kFieldRules::lead_rule of a structure without a lead rule (the first member that can battle).
NO_LEAD_RULE = NONE16

## Key of the field actions in structure.json (F2).
FIELD_ACTIONS_KEY = "field_actions"
## Target kinds in capsule::script::ActionTarget order ("none": a menu action anywhere).
TARGET_KINDS = ["none", "behavior", "object", "tag"]
## Most field actions (capsule::script::kMaxFieldActions).
MAX_FIELD_ACTIONS = 32
## Most game references (flags / variables) all field actions read together (capsule::script::kMaxFieldActionRefs).
MAX_GAME_REFS = 64
## Longest prompt (letters).
MAX_PROMPT_CHARS = 120
## The prompt of an asking action without its own ("{label}" is the action's label).
DEFAULT_PROMPT = "Use {label}?"
## The implicit variable the dispatcher writes the member's party slot to before an action's script runs.
MEMBER_VAR = "field_action_member"
## The implicit variable an asking action's wrapper keeps the answer in.
ANSWER_VAR = "field_action_answer"
## Prefix of an asking action's implicit wrapper script (the question, then the action's script).
ASK_SCRIPT_PREFIX = "field_action_ask_"
## Prefix of a move's input in a requirement (1 when the member knows it).
KNOWS_PREFIX = "knows."
## The member's passive input (0 = none, else passive index + 1).
PASSIVE_INPUT = "passive"
## Prefix of a passive's word (its index + 1).
PASSIVE_PREFIX = "passive."
## Game reference kinds (capsule::script::FieldActionRef::kind order): a script flag, a script variable.
GAME_REF_KINDS = ["flag", "var"]
## Range of a flag reference.
FLAG_RANGE = (0, 1)
## Range of a variable reference (any script variable: u32 variables stop at 2^31 - 1).
VAR_RANGE = (0, (1 << 31) - 1)
## Regex groups of a game reference: its kind, its name.
REF_KIND_GROUP, REF_NAME_GROUP = 1, 2
## A game reference in a requirement's text: flag.NAME or var.NAME (not after a letter, digit, '_' or '.').
GAME_REF = re.compile(r"(?<![A-Za-z0-9_.])(flag|var)\.([A-Za-z_][A-Za-z0-9_]*)")
## FieldActionInfo::flags bit: the action asks first.
ASKS_FLAG = 1
## kFieldRules::first_action_rule without field actions.
NO_ACTION_RULE = NONE16

## Key of the step effects in structure.json (F3).
STEP_EFFECTS_KEY = "step_effects"
## Most step effects (capsule::script::kMaxStepEffects: a due bit each).
MAX_STEP_EFFECTS = 16
## Most steps between two runs of a step effect (its counter is a u16 variable).
MAX_EVERY = 0xFFFF
## Damage rules in capsule::script::StepDamage order.
DAMAGE_RULES = ["faint", "survive"]
## The damage rule of a step effect that names none.
DEFAULT_DAMAGE = "faint"
## The moments a step effect's messages may name (capsule::script::StepEffectInfo::messages order).
STEP_MOMENTS = ["hurt", "survived", "fainted"]
## A member's outcome of one run (capsule::battle::StepOutcome order; the outcome variable's values).
STEP_OUTCOMES = ["none", "hurt", "survived", "fainted"]
## Prefix of a step effect's implicit script variable (the steps counted towards its next run).
STEP_VAR_PREFIX = "step_effect_"
## The implicit variable a step effect's script reads the member from (its party slot).
MEMBER_STEP_VAR = "step_effect_member"
## The implicit variable a step effect's script reads the worst outcome from (STEP_OUTCOMES index).
OUTCOME_STEP_VAR = "step_effect_outcome"
## Names a step effect may not take (their implicit variables would be the two above).
RESERVED_STEP_NAMES = {"member", "outcome"}

register_keys({"field_effect": {"name", "label", "unit", "stack", "expiry", "persists"},
               # C9: whether the lead rules see eggs (tools/battlec/nursery.py reads it)
               "lead": {"when", HOOK_WHEN_KEY, "allow_eggs"},
               "field_action": {"name", "label", "requires", "target", "ask", "prompt", "script",
                                # B2: whether an egg may do it (tools/battlec/nursery.py reads it)
                                "allow_eggs"},
               "field_action_target": set(TARGET_KINDS[1:]),
               # C9: whether its handlers run on eggs (tools/battlec/nursery.py reads it)
               "step_effect": {"name", "label", "every", "damage", "messages", "script", "allow_eggs"},
               "step_effect_messages": set(STEP_MOMENTS)})


def var_name(name: str) -> str:
   """!The implicit script variable of a field effect.
   @param name  the effect's name
   @return the variable's name
   """
   return VAR_PREFIX + name


def end_script_name(name: str) -> str:
   """!The implicit expiry script of a field effect.
   @param name  the effect's name
   @return the script's name
   """
   return END_SCRIPT_PREFIX + name


def compile_field_effects(src: Json, where: str) -> list[dict[str, Any]]:
   """!The structure's timed field effects.
   @param src    structure.json field_effects (None: none)
   @param where  location (the structure manifest)
   @return [{"name", "label", "unit", "stack", "expiry" (text or None), "persists", "unit_id", "stack_id", "flags"}]
   """
   if src is None:
      return []
   if not isinstance(src, list) or len(src) > MAX_FIELD_EFFECTS:
      raise ManifestError(f"{where}: {FIELD_EFFECTS_KEY} must be a list of at most {MAX_FIELD_EFFECTS} records")
   out = []
   seen: set[str] = set()
   for i, e in enumerate(src):
      w = f"{where}: {FIELD_EFFECTS_KEY}[{i}]"
      check_keys(e, "field_effect", w)
      name = e.get("name")
      if not isinstance(name, str) or not NAME.fullmatch(name) or name in seen:
         raise ManifestError(f"{w}: needs a name of its own (a lower-case identifier)")
      seen.add(name)
      label = e.get("label", name)
      if not isinstance(label, str) or not 1 <= len(label) <= MAX_LABEL_CHARS:
         raise ManifestError(f"{w}.label: 1 to {MAX_LABEL_CHARS} letters")
      unit = e.get("unit")
      if unit not in UNITS:
         raise ManifestError(f"{w}.unit: one of {', '.join(UNITS)}")
      stack = e.get("stack", DEFAULT_STACK)
      if stack not in STACKS:
         raise ManifestError(f"{w}.stack: one of {', '.join(STACKS)}")
      expiry = e.get("expiry")
      if expiry is not None and (not isinstance(expiry, str) or not 1 <= len(expiry) <= MAX_EXPIRY_CHARS):
         raise ManifestError(f"{w}.expiry: a message of 1 to {MAX_EXPIRY_CHARS} letters")
      persists = e.get("persists", True)
      if not isinstance(persists, bool):
         raise ManifestError(f"{w}.persists: true or false")
      out.append({"name": name, "label": label, "unit": unit, "stack": stack, "expiry": expiry,
                  "persists": persists, "unit_id": UNITS.index(unit), "stack_id": STACKS.index(stack),
                  "flags": PERSISTS_FLAG if persists else 0})
   return out


def step_var_name(name: str) -> str:
   """!The implicit variable of a step effect (F3: the steps counted towards its next run).
   @param name  the effect's name
   @return the variable's name
   """
   return STEP_VAR_PREFIX + name


def compile_step_effects(src: Json, messages: Sequence[str | None], where: str) -> list[dict[str, Any]]:
   """!The structure's step effects (F3).
   @param src       structure.json step_effects (None: none)
   @param messages  the battle's messages at their ids (battlec/texts.py declared_messages(); None: an optional one
                    left out)
   @param where     location (the structure manifest)
   @return [{"name", "label", "every", "damage", "messages" ({moment: key}), "script" (or None), "damage_id",
           "message_ids" (STEP_MOMENTS order, NONE8 for none)}]
   """
   if src is None:
      return []
   if not isinstance(src, list) or len(src) > MAX_STEP_EFFECTS:
      raise ManifestError(f"{where}: {STEP_EFFECTS_KEY} must be a list of at most {MAX_STEP_EFFECTS} records")
   known = [m for m in messages if m is not None]
   out = []
   seen: set[str] = set()
   for i, e in enumerate(src):
      w = f"{where}: {STEP_EFFECTS_KEY}[{i}]"
      check_keys(e, "step_effect", w)
      name = e.get("name")
      if not isinstance(name, str) or not NAME.fullmatch(name) or name in seen:
         raise ManifestError(f"{w}: needs a name of its own (a lower-case identifier)")
      if name in RESERVED_STEP_NAMES:
         raise ManifestError(f"{w}: {name!r} is taken (the implicit variable {step_var_name(name)} is the step "
                             f"effects' own)")
      seen.add(name)
      label = e.get("label", name)
      if not isinstance(label, str) or not 1 <= len(label) <= MAX_LABEL_CHARS:
         raise ManifestError(f"{w}.label: 1 to {MAX_LABEL_CHARS} letters")
      every = e.get("every")
      if isinstance(every, bool) or not isinstance(every, int) or not 1 <= every <= MAX_EVERY:
         raise ManifestError(f"{w}.every: the steps between two runs, 1 to {MAX_EVERY}")
      damage = e.get("damage", DEFAULT_DAMAGE)
      if damage not in DAMAGE_RULES:
         raise ManifestError(f"{w}.damage: one of {', '.join(DAMAGE_RULES)}")
      msgs = e.get("messages", {})
      check_keys(msgs, "step_effect_messages", f"{w}.messages")
      ids = []
      for moment in STEP_MOMENTS:
         key = msgs.get(moment)
         if key is None:
            ids.append(NONE8)
            continue
         if not isinstance(key, str) or key not in known:
            raise ManifestError(f"{w}.messages.{moment}: {key!r} is not one of the battle's messages")
         ids.append(list(messages).index(key))
      script = e.get("script")
      if script is not None and (not isinstance(script, str) or not script):
         raise ManifestError(f"{w}.script: the name of a game script (or leave it out)")
      out.append({"name": name, "label": label, "every": every, "damage": damage, "messages": dict(msgs),
                  "script": script, "damage_id": DAMAGE_RULES.index(damage), "message_ids": ids})
   return out


def compile_lead(src: Json, scope: WhenScope, rules: list[dict[str, Any]], where: str) -> int:
   """!The structure's lead rule, appended to the growth rules (rules::growth_when() evaluates it).
   @param src    structure.json lead (None: the first member that can battle)
   @param scope  the growth rules' WhenScope (the member's roles, stats, species fields)
   @param rules  the growth rule list (the rule is appended)
   @param where  location (the structure manifest)
   @return its rule id, or NO_LEAD_RULE
   """
   if src is None:
      return NO_LEAD_RULE
   w = f"{where}: {LEAD_KEY}"
   check_keys(src, "lead", w)
   text = src.get("when")
   if text is None and (src.get(HOOK_WHEN_KEY) is not None or "allow_eggs" in src):
      return NO_LEAD_RULE   # C8: only the hooks' member (C9: or the egg flag) is stated; the default lead stays
   if not isinstance(text, str) or not text.strip():
      raise ManifestError(f"{w}.when: an expression over a party member (the first one it holds for leads)")
   rule = compile_rule(text, [LEAD_TRIGGER], scope, w)
   rule["owner"] = LEAD_KEY
   rules.append(rule)
   return len(rules) - 1


def compile_hook_lead(src: Json, scope: WhenScope, rules: list[dict[str, Any]], where: str) -> int:
   """!C8: the lead section's `hook_when`: which member's passive and held item run the encounter hooks (`encounter`,
   `encounter_made`), when it is not the lead (`when`, whose level the pipelines and the global rules' holder
   are: a repel-style check). E.g. passives and held items that work from the first party slot even when it fainted
   ("1"), while a repel-style rule compares with the first member that can battle (the default lead).
   @param src    structure.json lead (None: none)
   @param scope  the growth rules' WhenScope
   @param rules  the growth rule list (the rule is appended)
   @param where  location (the structure manifest)
   @return its rule id, or NO_LEAD_RULE (the lead runs them)
   """
   text = src.get(HOOK_WHEN_KEY) if isinstance(src, dict) else None
   if text is None:
      return NO_LEAD_RULE
   w = f"{where}: {LEAD_KEY}.{HOOK_WHEN_KEY}"
   if not isinstance(text, str) or not text.strip():
      raise ManifestError(f"{w}: an expression over a party member (the first one it holds for runs the encounter "
                          f"hooks of its passive and held item)")
   rule = compile_rule(text, [LEAD_TRIGGER], scope, w)
   rule["owner"] = LEAD_KEY
   rules.append(rule)
   return len(rules) - 1


def ask_script_name(name: str) -> str:
   """!The implicit wrapper script of an asking field action (its question, then its script).
   @param name  the action's name
   @return the script's name
   """
   return ASK_SCRIPT_PREFIX + name


def prompt_text(action: Mapping[str, Any]) -> str:
   """!The question an asking field action shows.
   @param action  a compile_field_action_records() record
   @return its prompt, or DEFAULT_PROMPT with its label
   """
   return action["prompt"] if action["prompt"] is not None else DEFAULT_PROMPT.format(label=action["label"])


def compile_field_action_records(src: Json, where: str) -> list[dict[str, Any]]:
   """!The structure's field actions without their requirements' rules (the script compiler's view; the battle
   compiler's compile_field_actions() adds the rules).
   @param src    structure.json field_actions (None: none)
   @param where  location (the structure manifest)
   @return [{"name", "label", "requires" (text or None), "target" (TARGET_KINDS name), "target_name" (or None),
           "ask", "prompt" (text or None), "script"}]
   """
   if src is None:
      return []
   if not isinstance(src, list) or len(src) > MAX_FIELD_ACTIONS:
      raise ManifestError(f"{where}: {FIELD_ACTIONS_KEY} must be a list of at most {MAX_FIELD_ACTIONS} records")
   out = []
   seen: set[str] = set()
   for i, a in enumerate(src):
      w = f"{where}: {FIELD_ACTIONS_KEY}[{i}]"
      check_keys(a, "field_action", w)
      name = a.get("name")
      if not isinstance(name, str) or not NAME.fullmatch(name) or name in seen:
         raise ManifestError(f"{w}: needs a name of its own (a lower-case identifier)")
      seen.add(name)
      label = a.get("label", name)
      if not isinstance(label, str) or not 1 <= len(label) <= MAX_LABEL_CHARS:
         raise ManifestError(f"{w}.label: 1 to {MAX_LABEL_CHARS} letters")
      requires = a.get("requires")
      if requires is not None and (not isinstance(requires, str) or not requires.strip()):
         raise ManifestError(f"{w}.requires: an expression over a party member and the game state")
      target, target_name = TARGET_KINDS[0], None
      t = a.get("target")
      if t is not None:
         check_keys(t, "field_action_target", f"{w}.target")
         if len(t) != 1 or not isinstance(next(iter(t.values())), str) or not next(iter(t.values())):
            raise ManifestError(f"{w}.target: one of {{{', '.join(TARGET_KINDS[1:])}: NAME}}, or null (a menu "
                                f"action)")
         target, target_name = next(iter(t.items()))
      ask = a.get("ask", True)
      if not isinstance(ask, bool):
         raise ManifestError(f"{w}.ask: true or false")
      prompt = a.get("prompt")
      if prompt is not None and (not isinstance(prompt, str) or not 1 <= len(prompt) <= MAX_PROMPT_CHARS):
         raise ManifestError(f"{w}.prompt: a question of 1 to {MAX_PROMPT_CHARS} letters")
      script = a.get("script")
      if not isinstance(script, str) or not script:
         raise ManifestError(f"{w}.script: the script it runs")
      out.append({"name": name, "label": label, "requires": requires, "target": target, "target_name": target_name,
                  "ask": ask, "prompt": prompt, "script": script})
   return out


def game_refs(actions: Sequence[Mapping[str, Any]]) -> list[tuple[str, str]]:
   """!The flags and variables the field actions' requirements read, by first appearance (the battle data reads
   reference k through rules::field_state(k); the script compiler emits the same list resolved).
   @param actions  compile_field_action_records() result
   @return [(GAME_REF_KINDS name, flag or variable name)]
   """
   out: list[tuple[str, str]] = []
   for a in actions:
      for m in GAME_REF.finditer(a["requires"] or ""):
         ref = (m.group(REF_KIND_GROUP), m.group(REF_NAME_GROUP))
         if ref not in out:
            out.append(ref)
   return out


def move_word(name: str) -> str:
   """!A move's input name in a requirement.
   @param name  the move's name
   @return "knows." and its lower-case identifier
   """
   return KNOWS_PREFIX + ident(name).lower()


def passive_word(name: str) -> str:
   """!A passive's word in a requirement.
   @param name  the passive's name
   @return "passive." and its lower-case identifier
   """
   return PASSIVE_PREFIX + ident(name).lower()


def extend_scope(scope: WhenScope, moves: Sequence[str], passives: Sequence[str], refs: Sequence[tuple[str, str]],
                 where: str) -> None:
   """!Adds what a requirement reads besides the growth inputs: knows.MOVE, passive with the passive words, and the
   game references (after the growth rules are compiled: growth `when` expressions do not read them).
   @param scope     the growth rules' WhenScope (changed)
   @param moves     the moves' names (index order)
   @param passives  the passives' names (index order)
   @param refs      game_refs() result
   @param where     location
   @return None
   """
   for k, m in enumerate(moves):
      word = move_word(m)
      if word not in scope.ranges:
         scope._add(word, FLAG_RANGE, ("knows", k))
   if PASSIVE_INPUT in scope.ranges:
      raise ManifestError(f"{where}: a role or input named {PASSIVE_INPUT!r} hides the field actions' passive input; "
                          f"rename it")
   scope._add(PASSIVE_INPUT, (0, len(passives)), ("passive",))
   for k, p in enumerate(passives):
      scope.constants.setdefault(passive_word(p), k + 1)
   for k, (kind, name) in enumerate(refs):
      scope._add(f"{kind}.{name}", FLAG_RANGE if kind == GAME_REF_KINDS[0] else VAR_RANGE, ("game", k))


def compile_field_actions(src: Json, scope: WhenScope, rules: list[dict[str, Any]], moves: Sequence[str],
                          passives: Sequence[str], where: str) -> dict[str, Any]:
   """!The structure's field actions with their requirements as growth rules (appended to rules, one per action in
   order: rule first_rule + k is action k's).
   @param src       structure.json field_actions (None: none)
   @param scope     the growth rules' WhenScope (extend_scope() adds the field inputs)
   @param rules     the growth rule list
   @param moves     the moves' names
   @param passives  the passives' names
   @param where     location (the structure manifest)
   @return {"actions" (compile_field_action_records() with "rule"), "first_rule" (or NO_ACTION_RULE), "refs"}
   @throws ManifestError for a requirement that does not parse, can leave int32 or can never hold
   """
   actions = compile_field_action_records(src, where)
   refs = game_refs(actions)
   if len(refs) > MAX_GAME_REFS:
      raise ManifestError(f"{where}: {FIELD_ACTIONS_KEY} read {len(refs)} flags and variables (at most "
                          f"{MAX_GAME_REFS})")
   if not actions:
      return {"actions": [], "first_rule": NO_ACTION_RULE, "refs": []}
   extend_scope(scope, moves, passives, refs, where)
   first = len(rules)
   for i, a in enumerate(actions):
      rule = compile_rule(a["requires"] or "1", [LEAD_TRIGGER], scope, f"{where}: {FIELD_ACTIONS_KEY}[{i}].requires")
      rule["owner"] = f"{FIELD_ACTIONS_KEY}:{a['name']}"
      rules.append(rule)
      a["rule"] = len(rules) - 1
   return {"actions": actions, "first_rule": first, "refs": refs}


def member_passive(c: Mapping[str, Any], record: Mapping[str, Any]) -> int:
   """!A record's passive as a requirement reads it (rules::member_passive(): its form's passives (C8: the record's
   form, growth::species_view(), as the C++ reads them), the slot named by the passive selector's low bits, an empty
   slot the first filled rolled slot's).
   @param c       compile_battle() result
   @param record  record dict ("species" name, "passive_selector", the form role)
   @return 0 for none, else the passive's index + 1
   """
   from .growth import form_of, form_view, species_index
   si = species_index(c, record)
   sp = form_view(c, si, form_of(c, record))
   slots = c.get("passive_slots") or []
   mask = (1 << max(len(slots) - 1, 0).bit_length()) - 1
   slot = int(record.get("passive_selector", 0)) & mask
   got = sp["passives"][slot] if slot < len(slots) and slot < len(sp["passives"]) else NONE8
   if got == NONE8:
      got = next((p for k, p in enumerate(sp["passives"]) if k < len(slots) and slots[k].get("rolled") and
                  p != NONE8), NONE8)
   return 0 if got == NONE8 else got + 1


def field_action_values(c: Mapping[str, Any], record: Mapping[str, Any],
                        game: Mapping[str, int] | None = None) -> dict[str, int]:
   """!Every input a field action's requirement reads, for a member and the game state (the Python mirror of
   rules::growth_when() with the field inputs; the studio's previews).
   @param c       compile_battle() result
   @param record  record dict ("moves": move names or indices)
   @param game    "flag.NAME" / "var.NAME" -> value (absent: 0)
   @return input name -> value
   """
   from .growth import when_values
   values = when_values(c, record, LEAD_TRIGGER)
   moves = c["names"]["moves"]
   known = {moves.index(m) if isinstance(m, str) else int(m) for m in (record.get("moves") or [])
            if not isinstance(m, str) or m in moves}
   for k, m in enumerate(moves):
      values[move_word(m)] = 1 if k in known else 0
   values[PASSIVE_INPUT] = member_passive(c, record)
   game = game or {}
   for kind, name in c["field"]["actions"]["refs"]:
      values[f"{kind}.{name}"] = int(game.get(f"{kind}.{name}", 0))
   return values


def action_member(c: Mapping[str, Any], party: Sequence[Mapping[str, Any]], action: str,
                  game: Mapping[str, int] | None = None) -> int | None:
   """!The first party member a field action's requirement holds for (battle::action_member()).
   @param c       compile_battle() result
   @param party   record dicts in party order
   @param action  the action's name
   @param game    the game state (field_action_values())
   @return its party slot, or None
   """
   from .growth import holds
   acts = c["field"]["actions"]["actions"]
   a = next((x for x in acts if x["name"] == action), None)
   if a is None:
      raise ManifestError(f"no field action {action!r}")
   for k, rec in enumerate(party):
      if holds(c, a["rule"], field_action_values(c, rec, game)):
         return k
   return None


def emit_field(field: Mapping[str, Any]) -> list[str]:
   """!The C++ of capsule::battle::kFieldRules (include/capsule/battle/data/field.h).
   @param field  {"lead_rule", "hook_lead_rule", "effects", "actions", "steps"} (compile_battle()'s "field")
   @return lines
   """
   acts = field["actions"]
   # F3: the step effect count takes the former reserved u16's first byte
   steps = len(field.get("steps", []))
   hooks = field.get("hook_lead_rule", NO_LEAD_RULE)   # C8: the hooks' member's rule
   lines = ["", "namespace capsule::battle {", "",
            f"const FieldRules kFieldRules = {{0x{field['lead_rule']:04X}, {len(field['effects'])}, "
            f"{len(acts['actions'])}, 0x{acts['first_rule']:04X}, {steps}, 0, 0x{hooks:04X}, 0}};",
            f"static_assert({len(field['effects'])} <= script::kMaxFieldEffects);",
            f"static_assert({len(acts['actions'])} <= script::kMaxFieldActions);"]
   if steps:
      lines.append(f"static_assert({steps} <= script::kMaxStepEffects);")
   return lines + ["", "} // namespace capsule::battle", ""]


def field_ids(field: Mapping[str, Any]) -> list[str]:
   """!The ids header's field effect constants (effect logic and the script bank use the same indices).
   @param field  compile_battle()'s "field"
   @return lines
   """
   lines = [f"inline constexpr uint8_t FIELD_EFFECT_{e['name'].upper()} = {k};   ///< Field effect {e['label']}."
            for k, e in enumerate(field["effects"])]
   lines.append(f"inline constexpr int NUM_FIELD_EFFECTS = {len(field['effects'])};")
   acts = field["actions"]["actions"]
   if acts:   # F2: only with field actions (the ids header of a battle without them stays as it was)
      lines += [f"inline constexpr uint8_t FIELD_ACTION_{a['name'].upper()} = {k};   ///< Field action {a['label']}."
                for k, a in enumerate(acts)]
      lines.append(f"inline constexpr int NUM_FIELD_ACTIONS = {len(acts)};")
   steps = field.get("steps", [])
   if steps:   # F3: only with step effects (as F2's field actions)
      lines += [f"inline constexpr uint8_t STEP_EFFECT_{e['name'].upper()} = {k};   ///< Step effect {e['label']}."
                for k, e in enumerate(steps)]
      lines.append(f"inline constexpr int NUM_STEP_EFFECTS = {len(steps)};")
   return lines + [""]
