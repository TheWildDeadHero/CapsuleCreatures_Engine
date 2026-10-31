#!/usr/bin/env python3
"""!Compile battle data (capsule.battle/1) with its ruleset into the battle
tables (capsule::battle::kData), the ruleset formulas (capsule::rules) and
the creature record layout (capsule::monster::kLayout).

    battle_compiler.py <battle.json> <out.cpp> [--ids <out.h>] [--depfile <out.d>]
                       [--code-page <page.json>] [--platform <name>] [--scripts <scripts.json>]

Texts (names, messages) are encoded with the game's code page
(tools/text_encoder.py; --code-page, default the engine's ASCII page);
placeholders such as {user} become FD nn (capsule::text::Arg). --scripts
names the game's scripts manifest: the field placeholders it declares
({player}, D-22 decision 6) may appear in messages, condition and presenter
texts as FD 80-FE, which battle::format_text() keeps for the printer.

--ids also writes a header of constexpr indices (species, moves, parties,
presenters, categories, formats, statuses, weathers, items, passives, traits,
types, stats) for game code.

--platform names the target (desktop, headless, gba, dreamcast; CMake passes
CAPSULE_PLATFORM): the data is checked against that platform's engine limits
(build_settings.PLATFORM_LIMITS: stats per creature, record bytes). Without
it the GBA's limits apply, the strictest.

Follows the manifest rule (P-1): editor data stripped, unknown keys rejected,
names resolved to numbers, sizes checked against the engine.

The battle data is two manifests (D-23 section 2, E5s; tools/battlec/manifest.py):
battle.json (`capsule.battle/1`) holds the content: ruleset, structure,
types, moves, passives, items, traits, species, presenters, parties,
messages, effects, texts and cues; the structure manifest it names under
"structure" (`capsule.structure/1`, structure.json) holds the game's battle
structure: monster_layout, stats, stages, rules, chart, categories,
statuses, volatiles, weathers, side_conditions, field_conditions,
move_flags, tags, growth, formats, turn, learn, exp_policy, generation and
terms. A key in the other file is refused, naming the right one; a
single-file manifest (the form before E5s) is refused with the migration
command (tools/battle_split.py). The sections read one merged view, and the
structure sections' errors name the structure manifest. Every list entry is
named by "name":
  - ruleset: path of a capsule.ruleset/2 manifest (formulas, stat formulas,
    points and the damage, self_hit, speed, accuracy and crit pipelines; see
    ruleset_compiler.py).
  - structure: path of the capsule.structure/1 manifest (relative to
    battle.json).
  - monster_layout: role entries in record order (see capsule/monster/monster.h
    and tools/battlec/layout.py) in {"party_size": n (1 to the platform's
    most, 6 GBA / 8 otherwise), "move_slots": n (1-8) (both required: no
    default, D-22 decision 8), "roles": [...]}. species_key, level,
    experience, moves and primary_status are required; the resources name
    the roles their pools persist in ("status_bits": "auto" or n: the
    primary_status bits of the status id, battlec/layout.py).
    {"role": name, "width": bits, "count": n}
    chooses an engine role's width (up to the engine's maximum) or the
    nickname's length; per-stat roles hold one element per stat, moves / pp /
    pp_ups one per move slot. {"custom": name, "width": bits, "count": n,
    "default": v} declares a custom role (at most 32; battle_ids.h ROLE_<NAME>,
    ID 0x80 + its index; scripts reach it with role_get / role_set).
  - stats: 1 to the platform's most stats (10 GBA, 32 otherwise) [{name,
    label (in-game text), formula (a ruleset stat_formulas name), pool,
    order, trait, transfer}]: exactly one pool stat (hit points) and
    one order stat (turn order, escape); see tools/battlec/stats.py.
  - stages (D-22 A7): [{name, kind stat | accuracy | evasion | crit, stat
    (kind stat), label, range [lo, hi], table ([num, den] per stage; odds
    for the crit stage) or formula (a named ruleset formula (value,
    stage))}] in stage order (tools/battlec/stats.py); the pipelines read
    them through stage_scale(), staged(), accuracy_staged(), crit_chance()
    and crit_out_of().
  - rules: stat_cap (STAT_CAP; default 65535), level_max
    (LEVEL_MAX; default 255), rank_max (highest party rank, default 1),
    self_hit {power (self_hit_preview's; effect `self_hit POWER` names its
    own; default 1), category (a damaging category; default the first)}
    (the defaults are neutral: the engine's limits, D-22 decision 8), fallback_move (move name or null:
    used, paying nothing, when no move's cost can be paid). Multipliers, the damage roll,
    the order of the modifiers and the crit stage rule are the ruleset's
    pipelines.
  - types: [{name, color}]; chart (D-22 A6): {tiers [{name, q8 | decisive
    zero | ko, message}], combine exponent | multiply | sequential, messages
    {above, below}, types_per_species, added_types, default (a tier),
    matchups {attacker: {defender: tier name or multiplier}}}
    (tools/battlec/chart.py).
  - categories: 1-4 [{name, offense, defense}] (stat names; a category
    without them is non-damaging). Moves name one; move_is() tests it.
  - Conditions (D-22 A3-A5; their declarative fields are templates the
    compiler expands into effect code, tools/battlec/conditions.py):
    statuses: [{name, texts {inflicted, residual, skip, cured, already},
    residual, skip, cure, turns [min, max], immune_types, escalating (true
    or {cap}; default cap 255), thawed_by_move (a move flag), thawed_by_hit (type),
    cured_on_switch, color, tags, handlers (point handlers)}] (at most 63);
    volatiles: [{name, duration [min, max] | "turn" | "until_switch",
    counts before_move | end_of_turn, counter, word, clears_on [switch_out,
    faint], passes, skip (true or odds), chain {den, cap, kept_by (a move
    flag)}, texts {slot: text or {"message": NAME}}, hud, tags}] (at most 32;
    8 counters, 4 words in all); weathers: [{name, texts {started,
    continues, ended, hurts}, residual, immune_types, turns, tags, handlers
    (point handlers)}]; side_conditions: [{name, turns, layers, texts,
    tags}] and field_conditions: [{name, turns, texts, tags}] (16 each).
  - move_flags: [names] (at most 16): moves' "flags" name them, effect code
    tests them (`move.has(flag)`); tags: [names] (at most 16).
  - resources (D-25, tools/battlec/resources.py): [{name, label, scope
    battler | move, stat (battler: its maximum), base (move: the move field
    holding its base), max (move: a ruleset formula of (base, raises);
    default the base), raises (move: the role of the slots' raises), persist
    (the record role it is kept in, wide enough for the pool's highest
    value; default: refilled every battle), empty faint | none, regen (a
    number within int16 or a formula of (value, max), applied in the turn's
    regen end-of-turn phase)}] in payment order: exactly one battler
    resource "empty": "faint" on the pool stat (hit points); at most 4
    battler (hit points included) and 2 move resources on the GBA, 8 and 4
    otherwise.
  - passive_slots (D-25 decision 6): [{name, rolled (default true)}], the
    passive slots per species (default one rolled slot; at most 4 on the
    GBA, 8 otherwise).
  - moves: [{name, type, category (default the first), power, accuracy
    (percent or null = never misses), the base field of every move resource
    (e.g. pp), cost {resource: amount or formula of (value, max)} (default:
    1 of every move resource; a number cost a pool can reach, a hit point cost
    below the most hit points: paying never faints), priority, target, flags
    (names of move_flags), crit_stage}]. A move whose cost cannot be paid
    cannot be chosen (player or AI); rules.fallback_move states no cost.
  - passives, items: [{name, announce (default true: the owner is shown when
    its logic says `announce;`), handlers (point handlers)}].
  - G2 (tools/battlec/inventory.py, D-26 section 2): structure pockets
    [{name, label, capacity, quantity_max (default 99), sort added | index |
    name}] (at most 8 pockets and 256 slots on the GBA, 16 and 1024
    otherwise); items also take pocket, price, use none | field | battle |
    both (needing an `on use_field` / `on use_battle` handler), consumed,
    holdable (both default to not key), key (never consumed or sold) and
    target none | member | move_slot | battler (battle use only); battle.json
    shops [{name, items}]. Emitted as capsule::inventory::kInventory;
    battle_ids.h POCKET_<NAME> / SHOP_<NAME>.
  - G4 (tools/battlec/growth.py, D-26 section 4): structure transformation
    {context_values [{name, values | range, default}] (time_of_day: the
    clock port), cancel [triggers], blocked (an expression), learn,
    form_role} and training {gain (a named formula of (yield, value, total,
    level)), stat_cap, total_cap, yields {stat: species field}}; species
    transforms [{into, when, on, takes_item, also}] and forms [{name,
    types, base, passives, front, back, when, on}] ("base": the rule back to
    the species' own form). Emitted as capsule::growth::kGrowth with
    rules::growth_when() / train_gain(); battle_ids.h TRIGGER_ / CONTEXT_ /
    FORM_.
  - effects: the `.ceffect` sources (paths relative to the manifest; each
    must exist) with the logic of the moves, passives, items and conditions:
    owner blocks
    of the effect language (tools/effect_compiler.py; hooks, move phases and
    `point` handlers, see tools/battlec/logic.py). A damaging move without
    logic hits each target once. texts {name: text with {user} / {target}}
    (`text NAME;`), cues [names] (`cue NAME;`) and tags [names] (`tags
    [...]`) are the names that logic uses.
  - A manifest point handler is {"point": name, "code": "[when EXPR] {
    modify by 1.5; }"}: the same text as a `point` member of the owner's
    `.ceffect` block (tools/battlec/pipelines.py).
  - traits: [{name, up, down, pct [raised %, lowered %]}] (pct required
    with up or down: no default percentages).
  - growth: {name: expression in n} (experience for level n).
  - species: [{name, types, base [one per stat, 1-65535], passives [one per passive slot], growth, exp_yield
    (default 0), front, back, learnset [[level, move], ...]}].
  - formats: [{name, sides [[positions...], ...], range all|adjacent,
    layout [[x, y] per position], stands [[x, y] per side] (where presenters
    stand; default each side's first position), party_max, on_target_fainted
    redirect|fail}].
  - turn (D-22 A11, tools/battlec/turn.py; every key optional, the defaults
    are the engine's former fixed order): brackets {run, switch, item (-64 to
    63; bigger acts first), move ("priority": each move's own, or one
    bracket for every move)}, tie random_byte (a byte drawn per battler each
    turn) | position, end_of_turn [{run weather | battlers | sides | field
    | regen (each active battler's resource regen), sources (battlers: the
    dispatch sources whose residual hooks run, in order; default
    effect_order), each battler | once (sides / field: the
    residual for each of the condition's battlers or once)}] (weather, sides
    and field once each, battlers at least once; at most 8), effect_order
    (every hook dispatch source once: status, volatile, side, field,
    weather, passive, item), switching {send_out foes_first | position,
    switch_in speed | send_out (the order of the battle start's switch-in
    hooks)}.
  - Progression (D-22 A8 / A12, tools/battlec/progression.py; neutral
    defaults, D-22 decision 8: learn.default_moves and exp_policy.share are
    required, the rest default to on_full skip, receivers human_side,
    outsider never and no generation list): learn
    {on_full skip | request (a human party is asked which move to forget,
    RequestKind::LearnMove) | replace_oldest, default_moves last_n_by_level
    | first_n}; exp_policy {share faced | active | party, receivers
    human_side | opposing_sides, outsider never | owner_id (exp_gain()'s
    outsider is 1 for records whose owner_id is not Setup::owner_id)} (the
    gain itself is the ruleset's exp_gain()); generation [{role,
    random_range [lo, hi] (hi may be "max" or, for trait and
    passive_selector, "last") | constant v | derived expression (range)}]
    in draw order (default: none, nothing is drawn; never a resource's
    persist role: a new record's pools are full).
  - presenters: [{name, sprite ("sprite:X", optional), at [dx, dy], enter
    {style none|slide|fade|sequence, frames 1-255, from side|left|right|top|
    bottom, sequence path}, exit {the same styles and stay, frames, from,
    when send_out|after_intro, sequence}, returns, intro, defeat, victory}]:
    who shows a party on screen. Texts take {party}. Sequences
    (capsule.sequence/1) may only use sprite slots 14-15 and the ops in
    PRESENTER_SEQUENCE_OPS.
  - parties: [{name, presenters [up to 2 names], reward (reward base), rank
    (0..rank_max, default 1), escape (false: foes cannot run), ai [flags],
    members [{species, level, moves, item, passive (a passive slot's index
    or name), genes, trait}]}].
  - messages: {key: text} for every engine message the engine says
    (REQUIRED_MESSAGES); the others (OPTIONAL_MESSAGES) only where the data
    or logic names them; any other key (a lower-case identifier) declares a
    message of the battle's own, after the engine's ids (game_messages(),
    C4). Every
    message may use {user} and {target}; a message that carries a value also
    takes that one placeholder (MESSAGE_ARGS: {move}, {value}, {stat},
    {party}, {passive} or {item}). Status texts take {user} and {status};
    weather texts take {user}. An empty party_challenge says nothing.
  - terms: {concept: "Word" or {"one": "Word", "many": "Words"}, "plural":
    "english" | "same"} (TERMS lists the concepts and their defaults; a
    missing many form is plural_of() the word by the plural rule: English
    endings and irregular words by default, or the word itself). Messages, status, weather and
    presenter texts write {@concept} or {@concept:many}; terms are expanded
    at compile time and nothing about them reaches the ROM.

Limits that keep one engine step small (see capsule::battle::kEventsPerStep):
the effect compiler's event budgets (a hook program, a segment between
yields), at most MAX_LEARN_PER_LEVEL learnset moves at one level. A status
with turns skips every turn until it ends and takes no skip or cure odds.

The ruleset must accept everything the engine passes (check_formula_ranges,
bind_pipelines): each input's declared range covers the engine's values
(genes, training and awakening up to the layout's widths, levels up to
level_max, the data's base stats and yields, the speed pipeline's speeds for
escape_odds...), the stat formulas declare output ranges within 16 bits, and
every pipeline is analysed over this battle's stats, moves and tables
(ruleset_compiler.bind: named formula arguments inside their declared
ranges, outputs inside what the engine stores).

This file is the driver and the command line; the sections are the modules of
tools/battlec (layout, stats, chart, conditions, rules, progression, turn,
resources, presenters, content, pipelines, texts, emit).
"""
from __future__ import annotations

import re  # noqa: F401
import sys
from collections.abc import Iterable, Mapping, Sequence  # noqa: F401
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_settings  # noqa: E402
import effect_compiler as ec  # noqa: E402
import ruleset_compiler as rc  # noqa: E402
import sequence_compiler as seqc  # noqa: E402, F401
import text_encoder  # noqa: E402
from sequence_compiler import asset_key  # noqa: E402, F401

# The sections live in tools/battlec; every public name stays reachable as battle_compiler.<name>.
from battlec.common import (PLACEHOLDERS, U8_MAX, U16_MAX, I16_MAX, NONE8, NONE16, CODE_NEWLINE, CODE_END, Q8_ONE,
                            Q8_MAX, HALVES, FLOAT_DENOMINATOR_LIMIT, PERCENT, PAIR, I8_MIN, I8_MAX, Json, Sources,
                            Lookups, ManifestError, strip, check_keys, integer, fraction, q8, odds, encode, ident,
                            Names, KEYS, register_keys, Texts, GENERATOR_NAME)  # noqa: E402, F401
from battlec.layout import (DEFAULT_STAT_COUNT, DEFAULT_MOVE_SLOTS, MAX_MOVE_SLOTS, DEFAULT_PARTY_SIZE, COUNT_RULES,
                            ROLES, ROLE_NAMES, RETIRED_ROLES, ROLE_MAX_WIDTHS, ROLE_MAX_COUNTS, REQUIRED_ROLES,
                            MAX_RECORD_BYTES, MAX_PARTY_SIZE, MAX_CUSTOM_ROLES, CUSTOM_ROLE_BASE, MAX_CUSTOM_COUNT,
                            ROLE_ABSENT, FNV_OFFSET_BASIS, FNV_PRIME, U32_MASK, HASH_STAT_COUNT_TAG, HASH_WIDTH_TAG,
                            HASH_COUNT_TAG, HASH_CUSTOM_TAG, HASH_PARTY_SIZE_TAG, BITS_PER_BYTE, BITS_PER_WORD,
                            BYTES_PER_WORD, PARTY_SIZE, name_hash, role_width_max, role_count_max, role_max_value,
                            role_info, compile_layout, check_layout_fits, require_counts,
                           LAYOUT_REQUIRED)  # noqa: E402, F401
from battlec.texts import (MESSAGES, TERMS, INVARIANT_TERMS, PLURAL_SUFFIX, PLURAL_KEY, PLURAL_RULES,
                           DEFAULT_PLURAL_RULE, IRREGULAR_PLURALS, MAX_TERM_CHARS, MESSAGE_BASE_ARGS, MESSAGE_ARGS,
                           OPTIONAL_MESSAGES, REQUIRED_MESSAGES, ABSENT_MESSAGE, plural_of, compile_terms,
                           compile_messages, declared_messages, game_messages, MAX_MESSAGES, MESSAGE_KEY,
                           RETIRED_MESSAGES)  # noqa: E402, F401
from battlec.stats import (STAGE_KINDS, SINGLE_STAGE_KINDS, STAGE_LIMIT, STATS, STAT_NAME, MAX_CATEGORIES,
                           CATEGORY_DAMAGING, STATINFO_TRAIT, MAX_STAGE_DELTA,
                           preview_stats, compile_stats, compile_stages, stage_lookup, stage_of_kind,
                           shared_stat_stage, stage_battle_values, stat_index, compile_categories, is_damaging,
                           compile_category_by_type, compile_traits, compile_hit_category_by_type,
                           CATEGORY_BY_TYPE_KEY, HIT_CATEGORY_KEY)  # noqa: E402, F401
from battlec.chart import (MAX_TYPES_PER, MAX_TIERS, TIER_KINDS, COMBINE_MODES, DEFAULT_COMBINE, types_mask,
                           compile_types, compile_tiers, compile_chart, chart_battle_values,
                           type_effect, DECISIVE)  # noqa: E402, F401
from battlec.turn import (MAX_PARTIES, MAX_POSITIONS, MIN_SIDES, MAX_SIDES, ADJACENT_SIDES, MAX_BATTLERS,
                          ESCAPE_ATTEMPTS_MAX, compile_formats, compile_turn, DEFAULT_TURN, SOURCES, END_PHASES,
                          TIE_RULES, SEND_ORDERS, SWITCH_IN_ORDERS, PHASE_EACH, MAX_END_PHASES, BRACKET_MIN,
                          BRACKET_MAX, FORMAT_RANGES, ON_TARGET_FAINTED, EACH_PHASES, ONCE_PHASES,
                          AT_MOST_ONCE_PHASES)  # noqa: E402, F401
from battlec.conditions import (VOLATILES, MOVE_FLAGS, MAX_TURNS, MAX_STATUSES, MAX_VOLATILES, MAX_COUNTERS,
                                MAX_WORDS, MAX_SIDE_CONDS, MAX_FIELD_CONDS, MAX_MOVE_FLAGS, STATUS_TEXTS,
                                WEATHER_TEXTS, STATUS_TEXT_ARGS, WEATHER_TEXT_ARGS, CONDITION_TEXT, VOLATILE_TURN,
                                VOLATILE_SWITCH_OUT, VOLATILE_FAINT, VOLATILE_PASSES, VOLATILE_HUD,
                                CONDITION_TEXT_ARGS, CLEARS_ON, DURATIONS, COUNTS, DEFAULT_ESCALATION_CAP,
                                MAX_CHAIN_CAP, MAX_CHAIN_ODDS, MIN_CHAIN_DEN, VOLATILE_TEXT_SLOTS,
                                CONDITION_TEXT_SLOTS, VOLATILE_BOUND, BOUND_TO, MAX_BOUND,
                                compile_move_flags, compile_statuses, compile_weathers, compile_volatiles,
                                compile_side_conditions, compile_field_conditions, condition_entries,
                                own_text_table, status_template, weather_template,
                                volatile_template)  # noqa: E402, F401
from battlec.progression import (DEFAULT_RANK_MAX, MAX_REWARD, MAX_EXPERIENCE, DEFAULT_LEVEL_MAX, MIN_LEVEL_MAX,
                                 MAX_LEVEL_MAX, LEARN_ON_FULL, DEFAULT_MOVE_RULES, EXP_SHARES, EXP_RECEIVERS,
                                 EXP_OUTSIDERS, DEFAULT_LEARN, REQUIRED_LEARN, DEFAULT_EXP_POLICY,
                                 REQUIRED_EXP_POLICY, DEFAULT_GENERATION, GEN_KINDS, DRAWS_PER_KIND,
                                 GENERATED_BY_ENGINE, MAX_GENERATION, BOUND_MAX, BOUND_LAST, LAST_ROLES,
                                 DERIVED_ELEMENT,
                                 growth_names_of, compile_growth, compile_learn, compile_exp_policy,
                                 compile_generation, derived_inputs,
                                 run_derived, WIDE_ORDERS, DEFAULT_WIDE_ORDER, UNSTORED_KINDS, SPECIES_PREFIX,
                                 species_inputs, derived_values, rederive, record_inputs)  # noqa: E402, F401
from battlec.rules import (MOVED_TO_STAGES, DEFAULT_STAT_CAP, DEFAULT_SELF_HIT_POWER, STAT_INPUT_ROLES,
                           check_formula_ranges, compile_rules)  # noqa: E402, F401
from battlec.presenters import (MAX_PRESENTERS, MOTIONS, EDGES, EXIT_WHEN, DEFAULT_MOTION_FRAMES,
                                PRESENTER_EXIT_AFTER_INTRO, PRESENTER_STAYS, PRESENTER_RETURNS,
                                PRESENTER_SEQUENCE_SLOTS, PRESENTER_SEQUENCE_OPS, SPRITE_SLOT_OPS, STAGE_RESET_VISUALS,
                                STAGE_FLAGS_FIELD, PRESENTER_TEXT_ARGS, PRESENTER_TEXTS, STILL_MOTIONS, compile_motion,
                                compile_presenter_sequence, compile_presenter, compile_presenters)  # noqa: E402, F401
from battlec.content import (TARGETS, AI_FLAGS, DEFAULT_PASSIVE_SLOTS, DEFAULT_ROLLED, PARTY_NO_ESCAPE,
                             DEFAULT_RANK, MAX_MOVES, MAX_LEARNSET, MAX_PRIORITY,
                             DEFAULT_EXP_YIELD, MAX_LEARN_PER_LEVEL, compile_moves, logic_ops, logic_hints,
                             compile_holders, compile_species, compile_parties, compile_passive_slots,
                             passive_slot_bits, passive_rolled_mask, passive_slot_of, compile_species_fields,
                             MAX_SPECIES_FIELDS, MAX_SPECIES_FIELD_BITS, SPECIES_FIELD_NAME)  # noqa: E402, F401
from battlec.pipelines import (POINT_BASE, PointHandlers, bind_pipelines, catalog, compile_condition_handlers,
                               compile_logic_points, point_runner, move_type_of, category_of, point_handled,
                               OWNER_SOURCES, OWNER_KINDS, MANIFEST_HANDLER_LISTS)  # noqa: E402, F401
from battlec.logic import (LOGIC_KINDS, GLOBAL_KIND, GLOBAL_HOOKS, MAX_CUES, MAX_TAGS, STANDARD_HIT, DEFAULT_HIT,
                           NO_SUSPEND_HOOKS, TEMPLATE_SUFFIX, battle_catalog, compile_logic, load_sources, names_list,
                           split_points, texts_of)  # noqa: E402, F401
from battlec.resources import (SCOPES, EMPTY_RULES, NO_ROLE, STORAGE_MAX, ENGINE_STORAGE_ROLES, FORMULA_ARGS,
                               DEFAULT_COST, REGEN_PHASE, compile_resources, compile_cost, move_bases, check_formulas,
                               formula_ids, formula_dispatch, resource_names, pool_cap, check_persist_roles,
                               check_costs, pool_range, REGEN_RANGE, MAX_FORMULAS)  # noqa: E402, F401
from battlec.transfer import compile_transfer  # noqa: E402, F401
from battlec.ai import (RULES as AI_RULES, SWITCH_RULES as AI_SWITCH_RULES, RULE_ARGS as AI_RULE_ARGS,
                        SWITCH_ARGS as AI_SWITCH_ARGS, MAX_AI_PROFILES, MAX_AI_RULES,
                        DEFAULT_PROFILE as AI_DEFAULT_PROFILE, NO_VETO as AI_NO_VETO, AI_PIPELINE,
                        FLAG_RULES as AI_FLAG_RULES, compile_ai_profiles, flag_rules, party_profile)  # noqa: E402, F401
from battlec.delayed import (AT_USE as DELAYED_AT_USE, HIT_FLAGS as DELAYED_HIT_FLAGS,
                             DEFAULT_FLAGS as DELAYED_DEFAULT_FLAGS, DEFAULT_MISS_MESSAGE, MAX_ACCURACY as
                             DELAYED_MAX_ACCURACY, compile_delayed_hits)  # noqa: E402, F401
from battlec.manifest import (BATTLE_SCHEMA, STRUCTURE_SCHEMA, STRUCTURE_KEY, RULESET_KEY, DEFAULT_STRUCTURE_FILE,
                              STRUCTURE_KEYS, BATTLE_KEYS, RETIRED_KEYS, MIGRATION_COMMAND, read_json, read_parts,
                              read_battle, merge, split_manifest, structure_file_for, view_of,
                              sources_of)  # noqa: E402, F401
from battlec.emit import (emit, emit_ids, write_if_changed)  # noqa: E402, F401
from battlec.growth import (TRIGGERS, ITEM_TRIGGERS, UNCANCELLABLE, TIME_OF_DAY, CONTEXT_SOURCES, MAX_CONTEXT_VALUES,
                            MAX_TRANSFORMS, MAX_FORMS, BASE_FORM, TRAIN_GAIN_ARGS, MAX_TRAINING, STAT_PREFIX,
                            ITEM_PREFIX, TRIGGER_INPUT, ITEM_INPUT, WhenScope, compile_growth_rules, compile_rule,
                            compile_contexts, compile_training, compile_transformation, form_names_of, find_change,
                            when_values, training_gain, form_view, record_stats as growth_record_stats,
                            holds as growth_holds, DEFAULT_LEARN as GROWTH_DEFAULT_LEARN,
                            MAX_RULES as GROWTH_MAX_RULES)  # noqa: E402, F401
from battlec.inventory import (POCKET_NAME, SORT_RULES, DEFAULT_SORT, DEFAULT_QUANTITY_MAX, MAX_QUANTITY, ITEM_USES,
                               DEFAULT_USE, ITEM_TARGETS, DEFAULT_TARGET, FIELD_USES, BATTLE_USES, USE_HOOKS,
                               BATTLE_TARGETS, MAX_PRICE, ITEM_FIELDS, ITEM_FLAG_BITS, MAX_SHOPS, MAX_SHOP_ITEMS,
                               NO_POCKET, compile_pockets, item_fields, item_hooks, check_item_logic, compile_shops,
                               compile_inventory, check_held_items)  # noqa: E402, F401
# G3 (D-26 section 3): recruitment, the storage boxes and the AI's item-use rules
from battlec.recruit import (MAX_CHECKS as RECRUIT_MAX_CHECKS, MAX_BREAK_FREE as RECRUIT_MAX_BREAK_FREE,
                             MOMENTS as RECRUIT_MOMENTS, KEEPS as RECRUIT_KEEPS, OWNED as RECRUIT_OWNED,
                             DEFAULT_OWNED as RECRUIT_DEFAULT_OWNED, NO_STATUS as RECRUIT_NO_STATUS,
                             DEFAULT_STATUS_BONUS as RECRUIT_DEFAULT_STATUS_BONUS,
                             MAX_STATUS_BONUS as RECRUIT_MAX_STATUS_BONUS, MAX_BONUS as RECRUIT_MAX_BONUS,
                             RECRUIT_PIPELINE, RECRUIT_KEY, MESSAGE_MOMENTS as RECRUIT_MESSAGE_MOMENTS,
                             BREAK_FREE_MOMENT as RECRUIT_BREAK_FREE_MOMENT, REFUSALS as RECRUIT_REFUSALS,
                             compile_recruit,
                             message_args as recruit_message_args)  # noqa: E402, F401
from battlec.storage import (STORAGE_KEY, NUMBER_PLACEHOLDER as BOX_NUMBER_PLACEHOLDER,
                             DEFAULT_NAMES as BOX_DEFAULT_NAMES, MAX_NAME_CHARS as BOX_MAX_NAME_CHARS,
                             compile_storage, used_bytes as box_used_bytes)  # noqa: E402, F401
# F1 (D-27 sections 1 and 2): the lead rule and the timed field effects
from battlec.field import (LEAD_KEY, FIELD_EFFECTS_KEY, UNITS as FIELD_EFFECT_UNITS,
                           STACKS as FIELD_EFFECT_STACKS, DEFAULT_STACK as FIELD_EFFECT_DEFAULT_STACK,
                           MAX_FIELD_EFFECTS, MAX_LABEL_CHARS as FIELD_EFFECT_MAX_LABEL,
                           MAX_EXPIRY_CHARS as FIELD_EFFECT_MAX_EXPIRY, MAX_AMOUNT as FIELD_EFFECT_MAX_AMOUNT,
                           NO_LEAD_RULE, compile_field_effects, compile_lead, compile_hook_lead,
                           HOOK_WHEN_KEY as LEAD_HOOK_WHEN_KEY, var_name as field_effect_var,
                           end_script_name as field_effect_end_script)  # noqa: E402, F401
# F2 (D-27 section 3): the field actions
from battlec.field import (FIELD_ACTIONS_KEY, TARGET_KINDS as FIELD_ACTION_TARGETS, MAX_FIELD_ACTIONS,
                           MAX_PROMPT_CHARS as FIELD_ACTION_MAX_PROMPT, DEFAULT_PROMPT as FIELD_ACTION_DEFAULT_PROMPT,
                           MEMBER_VAR as FIELD_ACTION_MEMBER_VAR, KNOWS_PREFIX as FIELD_ACTION_KNOWS,
                           PASSIVE_INPUT as FIELD_ACTION_PASSIVE, GAME_REF_KINDS as FIELD_ACTION_GAME_REFS,
                           compile_field_actions, compile_field_action_records, field_action_values,
                           action_member, game_refs as field_action_refs, move_word as field_action_move_word,
                           passive_word as field_action_passive_word,
                           ask_script_name as field_action_ask_script, NO_ACTION_RULE,
                           LEAD_TRIGGER as FIELD_LEAD_TRIGGER)  # noqa: E402, F401
# F3 (D-27 section 4): the step effects
from battlec.field import (STEP_EFFECTS_KEY, MAX_STEP_EFFECTS, MAX_EVERY as STEP_EFFECT_MAX_EVERY,
                           DAMAGE_RULES as STEP_EFFECT_DAMAGE_RULES, DEFAULT_DAMAGE as STEP_EFFECT_DEFAULT_DAMAGE,
                           STEP_MOMENTS as STEP_EFFECT_MOMENTS, STEP_OUTCOMES as STEP_EFFECT_OUTCOMES,
                           MEMBER_STEP_VAR as STEP_EFFECT_MEMBER_VAR, OUTCOME_STEP_VAR as STEP_EFFECT_OUTCOME_VAR,
                           RESERVED_STEP_NAMES, STEP_VAR_PREFIX as STEP_EFFECT_VAR_PREFIX, compile_step_effects,
                           step_var_name as step_effect_var)  # noqa: E402, F401
from battlec.ai import (ITEM_RULES as AI_ITEM_RULES, ITEM_ARGS as AI_ITEM_ARGS, ITEM_KEY as AI_ITEM_KEY,
                        check_ai_items)  # noqa: E402, F401
from battlec.content import MAX_PARTY_ITEMS  # noqa: E402, F401
# B1 (D-28 section 1): the combination methods (re-exported for the studio)
from battlec.combine import (COMBINE_KEY, KINDS as COMBINE_KINDS, IMMEDIATE as COMBINE_IMMEDIATE,
                             DELIVERIES as COMBINE_DELIVERIES, AFTERWARDS as COMBINE_AFTERWARDS,
                             CHOOSERS as COMBINE_CHOOSERS, MOVE_SOURCES as COMBINE_MOVE_SOURCES,
                             RULE_KINDS as COMBINE_RULE_KINDS, PICK_CHOICES as COMBINE_PICK_CHOICES,
                             PICK_ELSE as COMBINE_PICK_ELSE, ORDER_INDEX as COMBINE_ORDER_INDEX,
                             TYPE_KEY as COMBINE_TYPE_KEY, MOMENTS as COMBINE_MOMENTS,
                             AGGREGATES as COMBINE_AGGREGATES, COUNT_INPUT as COMBINE_COUNT_INPUT,
                             INPUT_PARTS as COMBINE_INPUT_PARTS, CANDIDATE as COMBINE_CANDIDATE,
                             RESULT as COMBINE_RESULT, SPECIES_WORD as COMBINE_SPECIES_WORD,
                             TYPE_WORD as COMBINE_TYPE_WORD, MAX_METHODS as COMBINE_MAX_METHODS,
                             MIN_INPUTS as COMBINE_MIN_INPUTS, MAX_RULES as COMBINE_MAX_RULES,
                             MAX_TABLES as COMBINE_MAX_TABLES, MAX_CELLS as COMBINE_MAX_CELLS,
                             MAX_SOURCES as COMBINE_MAX_SOURCES, MAX_CANDIDATES as COMBINE_MAX_CANDIDATES,
                             MAX_COMBINE_MOVES, MAX_LABEL_CHARS as COMBINE_MAX_LABEL,
                             RESERVED_ROLES as COMBINE_RESERVED_ROLES, STATUSES as COMBINE_STATUSES,
                             DEFAULT_LEVEL as COMBINE_DEFAULT_LEVEL, FROM_KEY as COMBINE_FROM_KEY,
                             PUSH_KEY as COMBINE_PUSH_KEY, compile_combine, method_records,
                             slot_roles as combine_slot_roles, qualified_roles as combine_qualified_roles,
                             preview as combine_preview,
                             move_candidates as combine_move_candidates, method_of as combine_method_of,
                             input_values as combine_input_values)  # noqa: E402, F401
# B2 (D-28 sections 2 and 3): eggs and the nursery
from battlec.nursery import (EGGS_KEY, NURSERY_KEY, ALLOW_EGGS, EGG_MEMBER_VAR, DEFAULT_EGG_NAME,
                             MAX_NAME_CHARS as MAX_EGG_NAME_CHARS, DEFAULT_ODDS_SCALE as NURSERY_DEFAULT_ODDS,
                             DEFAULT_CYCLE_STEPS, NURSERY_KIND, EGG_DELIVERY, compile_eggs, compile_nursery,
                             check_nursery_methods, nursery_ids,
                             egg_record, hatch_steps, nursery_fee as nursery_fee_of,
                             nursery_gain as nursery_gain_of,
                             # C9: the transfer rule for eggs and the egg role's transfer key
                             TRANSFER_EGGS_KEY, TRANSFER_EGGS, EGG_TRANSFER_NAME, egg_transfer_key,
                             transfer_flags, EGG_FLAG_LEAD, EGG_FLAG_TRANSFORM,
                             EGG_FLAG_TRANSFER_HATCH)  # noqa: E402, F401

## A random generator's name (re-exported for the studio: a scenario's "rng", a declared generator).
RNG_NAME = GENERATOR_NAME
## The battler subjects effect logic names (holder, other, user, target; re-exported for the studio's logic
## editors, effect_compiler.BATTLER_SUBJECTS).
BATTLER_SUBJECTS = tuple(ec.BATTLER_SUBJECTS)
## Highest values of narrow fields (list sizes).
LIMITS = {"types": 32, "statuses": MAX_STATUSES, "passives": 255, "weathers": 255, "formats": 255, "growth": 255,
          "traits": 32, "moves": 0xFFFE, "species": 0xFFFE, "items": 0xFFFE, "parties": 0xFFFE, "presenters": 254,
          "categories": MAX_CATEGORIES}
## Most handlers (16-bit first-handler indices).
MAX_HANDLERS = 0xFFFF
## Most owner texts in all (ids below capsule::battle::kConditionText).
MAX_TEXT_IDS = CONDITION_TEXT - 1
## Exit status of a command line that is not understood.
EXIT_USAGE = 2
## Command-line words of an option with a value (the flag and its file).
OPTION_WORDS = 2
## Positional command-line arguments (battle.json and out.cpp).
POSITIONAL_ARGS = 2


def load_battle(path: str | Path, sources: Sources | None = None,
                deps: list[Path] | None = None) -> tuple[Json, Json, Path]:
   """!Reads a battle manifest, the structure manifest it names (one merged view, battlec/manifest.py) and its
   ruleset (no compiling).
   @param path     battle.json
   @param sources  {Path: data} replacing files (editors' unsaved documents)
   @param deps     list collecting the battle and structure manifests' paths, or None
   @return (merged battle data, ruleset data, ruleset path)
   """
   path = Path(path).resolve()
   b, _st_path = read_battle(path, sources, deps)
   if not isinstance(b.get(RULESET_KEY), str):
      raise ManifestError(f"{path.name}: ruleset must name a {rc.SCHEMA} file")
   rp = (path.parent / b[RULESET_KEY]).resolve()
   return b, read_json(rp, sources), rp


def compile_battle(path: str | Path, deps: list[Path] | None = None, sources: Sources | None = None, *,
                   codepage: text_encoder.CodePage = text_encoder.DEFAULT,
                   platform: str | None = None, scripts: str | Path | None = None) -> dict[str, Any]:
   """!Compiles a battle manifest, section by section (tools/battlec).
   @param path      battle.json
   @param deps      list collecting files read (depfile)
   @param sources   {Path: data} replacing files
   @param codepage  the game's code page (every text)
   @param platform  target platform whose limits apply (build_settings.PLATFORMS), or None for the GBA's
   @param scripts   the game's scripts manifest (capsule.scripts/1): the field placeholders it declares
                    ({player}, ...) may appear in the battle's in-game texts (FD 80-FE), or None
   @return compiled dict (see emit())
   """
   b, rs_data, rs_path = load_battle(path, sources, deps)
   if deps is not None:
      deps.append(rs_path)
   where = Path(path).name
   # the structure sections' errors name the structure manifest (E5s; read_battle() checked both files' keys)
   swhere = Path(b[STRUCTURE_KEY]).name
   try:
      ruleset = rc.load(rs_data, rs_path.name)
   except rc.RulesetError as e:
      raise ManifestError(str(e))
   try:
      limits = build_settings.platform_limits(platform)
   except build_settings.SettingsError as e:
      raise ManifestError(str(e))

   lists = {}
   for key in ("types", "categories", "statuses", "weathers", "moves", "passives", "items", "traits", "species",
               "formats", "presenters", "parties"):
      v = b.get(key, [])
      loc = swhere if key in STRUCTURE_KEYS else where
      if not isinstance(v, list):
         raise ManifestError(f"{loc}: {key} must be a list")
      lists[key] = v
      if len(v) > LIMITS[key]:
         raise ManifestError(f"{loc}: at most {LIMITS[key]} {key}")
   if not lists["types"]:
      raise ManifestError(f"{where}: needs at least one type")
   if not lists["formats"]:
      raise ManifestError(f"{swhere}: needs at least one format")
   if not lists["categories"]:
      raise ManifestError(f"{swhere}: needs at least one move category")
   n = {
      "types": Names("type", lists["types"], f"{where}: types"),
      "categories": Names("category", lists["categories"], f"{swhere}: categories"),
      "statuses": Names("status", lists["statuses"], f"{swhere}: statuses", first=1),
      "weathers": Names("weather", lists["weathers"], f"{swhere}: weathers"),
      "moves": Names("move", lists["moves"], f"{where}: moves"),
      "passives": Names("passive", lists["passives"], f"{where}: passives"),
      "items": Names("item", lists["items"], f"{where}: items", first=1),
      "traits": Names("trait", lists["traits"], f"{where}: traits"),
      "formats": Names("format", lists["formats"], f"{swhere}: formats"),
      "presenters": Names("presenter", lists["presenters"], f"{where}: presenters"),
      "parties": Names("party", lists["parties"], f"{where}: parties"),
   }
   terms = compile_terms(b.get("terms", {}), f"{swhere}: terms")
   # the engine messages the data can name: the required ones and the optional ones the manifest gives
   message_names = declared_messages(b.get("messages", {}))
   field = field_placeholders(scripts, codepage, where, deps)
   enc = Texts(codepage, terms, field)

   # The sections, in the order their checks run (an error names the first problem).
   stats = compile_stats(b.get("stats"), ruleset, rs_path.name, limits["max_stats"], swhere, enc)
   # D-25 decision 6: the passive slots per species are the manifest's (a slot may be rolled or only given)
   passive_slots = compile_passive_slots(b.get("passive_slots"), limits["max_passive_slots"], swhere)
   require_counts(b.get("monster_layout"), f"{swhere}: monster_layout")
   layout = compile_layout(b.get("monster_layout"), f"{swhere}: monster_layout", limits["max_record_bytes"],
                           len(stats), limits["max_party_size"], len(lists["statuses"]))
   # transfer keys (E2c): no two stats with one key, transfer_defaults naming stats (checked here too, not only
   # where a game's transfer table compiles)
   compile_transfer(layout, stats, swhere)
   stages = compile_stages(b.get("stages"), stats, ruleset, rs_path.name, limits["max_stats"], swhere, enc)
   n["stages"] = stage_lookup(stages, f"{swhere}: stages")
   categories = compile_categories(lists["categories"], stats, swhere)
   growth_src = b.get("growth", {})
   growth_names = growth_names_of(growth_src, swhere)
   rules = compile_rules(b.get("rules", {}), lists["categories"], categories, n, swhere)
   rules["turn"] = compile_turn(b.get("turn"), swhere)
   # D-25: the pools battlers spend or lose (hit points first among them: the faint resource)
   resources = compile_resources(b.get("resources"), stats, layout, ruleset, rs_path.name, rules["turn"], limits,
                                 swhere, enc)
   types = compile_types(lists["types"], where, enc)
   chart = compile_chart(b.get("chart"), n, len(types), swhere, message_names)
   move_flags = compile_move_flags(b, swhere)
   statuses = compile_statuses(lists["statuses"], n, swhere, enc, move_flags)
   weathers = compile_weathers(lists["weathers"], n, swhere, enc)
   volatiles = compile_volatiles(b.get("volatiles", []), swhere, enc, move_flags, message_names)
   side_conds = compile_side_conditions(b.get("side_conditions", []), swhere, enc, message_names)
   field_conds = compile_field_conditions(b.get("field_conditions", []), swhere, enc, message_names)
   tags = names_list(b, "tags", MAX_TAGS, swhere)
   # C6 (review4 finding 19): the tag whose holders suppress the weather (a weather-calming passive), or none
   suppress = b.get("weather_suppress_tag")
   if suppress is not None and suppress not in tags:
      raise ManifestError(f"{swhere}: weather_suppress_tag: one of the structure's tags ({', '.join(tags) or 'none'}), "
                          f"got {suppress!r}")
   weather_suppress_tags = 1 << tags.index(suppress) if suppress is not None else 0
   # G1: the delayed hits (effect `delay_hit`) and the AI profiles (a party's `ai`)
   delayed_hits = compile_delayed_hits(b.get("delayed_hits"), swhere, message_names)
   # F1: the timed field effects (effect `field_effect start NAME N`, `field.effect_active(NAME)`)
   field_effects = compile_field_effects(b.get(FIELD_EFFECTS_KEY), swhere)
   # F3: the step effects (effect `on steps`, `field.step_effect`; their messages are the battle's)
   step_effects = compile_step_effects(b.get(STEP_EFFECTS_KEY), message_names, swhere)
   # B1: the combination methods' names and slot roles (their rules compile after the growth rules)
   combine_records = method_records(b.get(COMBINE_KEY), limits["max_combine_inputs"], swhere)
   ai = compile_ai_profiles(b.get("ai_profiles"), where, list((rs_data.get("pipelines") or {}).keys()),
                            [e.get("name") for e in lists["items"]])
   # effect logic: the .ceffect sources, their point members apart (point handlers)
   positions: dict[int, tuple[int, int]] = {}
   files: dict[int, str] = {}
   logic, logic_points = split_points(load_sources(b.get("effects"), Path(path).resolve().parent, where, deps, sources,
                                                   positions, files))
   crit = stage_of_kind(stages, "crit")
   # E11: a structure whose types decide damaging moves' categories
   type_categories = compile_category_by_type(b.get("category_by_type"), lists["types"], lists["categories"],
                                              categories, swhere)
   # C5: the categories of hit types that are not a move's own (BattleData::hit_categories)
   hit_categories = compile_hit_category_by_type(b.get(HIT_CATEGORY_KEY), lists["types"], lists["categories"],
                                                 categories, type_categories, swhere)
   moves = compile_moves(lists["moves"], lists["categories"], categories, n, where, enc, logic, move_flags,
                         stages[crit]["range"][1] if crit != NONE8 else 0, resources, ruleset, rs_path.name,
                         type_categories)
   # E13a: the species' numeric data columns (derived expressions read them as species.NAME; E13b: effect logic
   # as species_field(NAME))
   species_fields = compile_species_fields(b.get("species_fields"), swhere)
   names = {**{k: [e["name"] for e in lists[k]] for k in lists}, "stats": [st["name"] for st in stats],
            "volatiles": [v["source_name"] for v in volatiles],
            "side_conditions": [sc["source_name"] for sc in side_conds],
            "field_conditions": [fc["source_name"] for fc in field_conds], "move_flags": move_flags, "tags": tags,
            "resources": resource_names(resources), "messages": message_names,
            "species_fields": [f["name"] for f in species_fields],
            "delayed_hits": [d["name"] for d in delayed_hits], "ai_profiles": ai["names"],
            "forms": form_names_of(lists["species"]), "field_effects": [e["name"] for e in field_effects],
            "step_effects": [e["name"] for e in step_effects],
            # B1: the combination methods and their slot roles (effect logic `field.combine_method`, `input.ROLE`)
            "combine_methods": [m["name"] for m in combine_records],
            "combine_slots": [{"name": r, "input": k} for r, k in combine_slot_roles(combine_records).items()] +
            # C9: method-qualified roles, `input.METHOD.ROLE`
            [{"name": r, "input": k} for r, k in combine_qualified_roles(combine_records).items()]}
   ph = PointHandlers(ruleset["points"], catalog(names, n["stages"]))
   handlers = []
   passives = compile_holders(lists["passives"], "passives", f"{where}: passives", enc, handlers, ph, logic_points)
   items = [None] + compile_holders(lists["items"], "items", f"{where}: items", enc, handlers, ph, logic_points)
   compile_condition_handlers(lists["statuses"], "statuses", statuses, ph, handlers, swhere, logic_points)
   compile_condition_handlers(lists["weathers"], "weathers", weathers, ph, handlers, swhere, logic_points)
   # E13b: the moves', volatiles' and side conditions' own point handlers (after the others: their ids stay)
   compile_logic_points(moves, "moves", names["moves"], ph, handlers, where, logic_points)
   compile_logic_points(volatiles, "volatiles", names["volatiles"], ph, handlers, swhere, logic_points)
   compile_logic_points(side_conds, "side_conditions", names["side_conditions"], ph, handlers, swhere, logic_points)
   if len(handlers) > MAX_HANDLERS:
      raise ManifestError(f"{where}: more than {MAX_HANDLERS} handlers")
   text_names, texts = texts_of(b, where, enc)
   # the volatile / side / field records' own texts follow the manifest's
   own_names, own_codes, own_ids = own_text_table(volatiles, side_conds, field_conds, len(texts))
   text_names, texts = text_names + own_names, texts + own_codes
   if len(texts) > MAX_TEXT_IDS:
      raise ManifestError(f"{where}: more than {MAX_TEXT_IDS} owner texts (with the condition records' own)")
   cues = names_list(b, "cues", MAX_CUES, where)
   stage_order = [nm for nm, _i in sorted(n["stages"].index.items(), key=lambda kv: kv[1])]
   conditions = condition_entries(statuses, weathers, volatiles, side_conds, field_conds, names, own_ids)
   effect_catalog = battle_catalog(names, stage_order, layout, ruleset["points"], text_names, cues, tags, conditions,
                                   move_flags)
   effects = compile_logic(names, moves, passives, items, statuses, weathers, logic, effect_catalog, handlers,
                           categories, where, {"volatiles": volatiles, "side_conditions": side_conds,
                                               "field_conditions": field_conds}, positions, files)
   check_delayed_phase(effects, rules["turn"], swhere)
   # G2: the bag's pockets, the items' bag and use fields (against their use_field / use_battle logic), shops
   inventory = compile_inventory(b, lists["items"], items, effects["bank"], limits, swhere, where, enc)
   check_held_items(lists["parties"], names["items"], inventory["items"], where)
   # G3: the AI's item-use rules name battle items (a hook rule's item has its own ai_score handler)
   check_ai_items(ai, inventory["items"], [set()] + [item_hooks(it, effects["bank"]) for it in items[1:]],
                  names["items"], where)
   traits = compile_traits(lists["traits"], stats, ruleset, rs_path.name, where, enc)
   growth = compile_growth(growth_src, growth_names, ruleset, rules["level_max"], swhere)
   species = compile_species(lists["species"], n, growth_names, rules["level_max"], where, enc,
                             [st["name"] for st in stats], chart["types_per_species"], passive_slots, species_fields)
   formats = compile_formats(lists["formats"], swhere, enc, layout["party_size"])
   presenters = compile_presenters(lists["presenters"], path, where, deps, codepage, terms, field)
   parties = compile_parties(lists["parties"], species, n, rules, where, enc, role_max_value(layout, "stat_genes"),
                             layout["move_slots"], layout["party_size"], passive_slots, ai)
   # chosen widths must hold what the data stores (the all-ones value of moves stays "no move")
   # (the resources' persist roles, current_hp among them, are checked against each pool's range below)
   check_layout_fits(layout, {"moves": len(moves), "held_item": len(items) - 1, "trait": len(traits) - 1,
                              "level": rules["level_max"], "passive_selector": len(passive_slots) - 1,
                              "experience": max((max(g) for g in growth), default=0)},
                     f"{swhere}: monster_layout")
   check_formula_ranges(ruleset, rs_path.name, rules, species, parties, layout, stats, traits)
   # the resources' formulas against the moves' bases and the pools, their persist roles against the pools' ranges,
   # the moves' costs against what a pool can hold, the free fallback (D-25)
   check_formulas(resources, moves, ruleset, rs_path.name, layout, rules["stat_cap"], stats)
   check_persist_roles(resources, moves, ruleset, layout, stats, swhere)
   check_costs(resources, moves, layout, rules, stats, where, lists["moves"], ruleset)
   # progression (D-22 A8 / A12): learn policy, experience policy, generation list (not the persisted pools)
   rules["progress"] = {"learn": compile_learn(b.get("learn"), swhere),
                        "exp": compile_exp_policy(b.get("exp_policy"), layout, swhere),
                        "generation": compile_generation(b.get("generation"), layout, ruleset, rules, len(traits),
                                                         swhere, [r["persist_name"] for r in resources
                                                                  if r["persist_name"] is not None],
                                                         species_fields)}
   # G3: recruitment (the recruit_odds pipeline's rate and status bonus inputs) and the storage boxes
   recruit = compile_recruit(b.get(RECRUIT_KEY), lists["statuses"], species_fields, layout, len(items),
                             message_names, list((rs_data.get("pipelines") or {}).keys()), swhere)
   storage = compile_storage(b.get(STORAGE_KEY), layout, limits, swhere, enc)
   # G4: transformation rules, forms and training values (battlec/growth.py; after the generation list: a
   # transformation re-derives the stored roles that read the species)
   transform = compile_growth_rules(b, layout, stats, species, species_fields, n, names["items"], ruleset, rs_path.name,
                                    rules, passive_slots, chart["types_per_species"],
                                    rules["progress"]["generation"], swhere, where)
   # F1: the lead rule (one more growth rule over a member's record)
   field = {"lead_rule": compile_lead(b.get(LEAD_KEY), transform["scope"], transform["rules"], swhere),
            "effects": field_effects, "steps": step_effects}
   # C8: the encounter hooks' member, when the structure picks another one than the lead
   field["hook_lead_rule"] = compile_hook_lead(b.get(LEAD_KEY), transform["scope"], transform["rules"], swhere)
   # F2: the field actions (one more growth rule each, over a member and the game state)
   field["actions"] = compile_field_actions(b.get(FIELD_ACTIONS_KEY), transform["scope"], transform["rules"],
                                            names["moves"], names["passives"], swhere)
   # B1: the combination methods (each slot's requirement one more growth rule, after the field actions': it reads
   # their words too)
   combine = compile_combine(b, layout, species, species_fields, names, transform, rules, limits, message_names,
                             enc, swhere, where)
   # B2: eggs (with the systems' refusal flags) and the nursery that runs the nursery method
   combine["eggs"] = compile_eggs(b, layout, species, species_fields, step_effects, combine, enc, swhere)
   # C9: the egg role transfers under the eggs' transfer name; a build without eggs still says what it does with one
   egg_transfer_key(layout, combine["eggs"], swhere)
   combine["transfer_flags"] = transfer_flags(b, swhere)
   combine["nursery"] = compile_nursery(b, combine, step_effects, limits, swhere)
   check_nursery_methods(combine, combine["nursery"], swhere)
   if len(transform["rules"]) > GROWTH_MAX_RULES:
      raise ManifestError(f"{where}: more than {GROWTH_MAX_RULES} growth rules (with the lead rule)")
   pipelines = bind_pipelines(ruleset, rs_path.name, rules, categories, stats, moves, stages, chart, recruit)
   messages = compile_messages(b.get("messages", {}), where, enc, recruit_message_args(recruit))
   return {"ruleset": ruleset, "layout": layout, "stats": stats, "stages": stages, "resources": resources,
           "passive_slots": passive_slots,
           "resource_formulas": formula_ids(resources, moves),
           "stage_names": [st["label"] for st in stages], "rules": rules,
           "types": types, "chart": chart, "categories": categories, "hit_categories": hit_categories,
           "statuses": statuses, "weathers": weathers,
           "volatiles": volatiles, "side_conditions": side_conds, "field_conditions": field_conds,
           "move_flags": move_flags,
           "moves": moves, "passives": passives, "items": items, "handlers": handlers,
           "traits": traits, "growth": growth, "growth_names": growth_names, "species": species,
           "species_fields": species_fields, "formats": formats,
           "presenters": presenters, "parties": parties, "messages": messages, "terms": terms,
           "pipelines": pipelines, "point_handlers": ph.entries, "effects": effects, "texts": texts,
           "effect_catalog": effect_catalog,
           "platform": platform or build_settings.DEFAULT_PLATFORM, "names": names, "inventory": inventory,
           "ai": ai, "delayed_hits": delayed_hits, "recruit": recruit, "storage": storage, "transform": transform,
           "weather_suppress_tags": weather_suppress_tags, "field": field, "combine": combine}


def check_delayed_phase(effects: Mapping[str, Any], turn: Mapping[str, Any], where: str) -> None:
   """!A battle whose logic uses `delay` needs the end-of-turn phase that lands it (E13b).
   @param effects  compile_logic() result
   @param turn     compiled turn
   @param where    location (the structure manifest)
   @return None
   @throws ManifestError without a delayed phase
   """
   delayed = END_PHASES.index("delayed")
   if any(e["phase"] == delayed for e in turn["end"]):
      return
   if any(name in ("Delay", "DelayHit") for _at, name, _v in ec.instructions(effects["bank"].code)):
      raise ManifestError(f"{where}: turn.end_of_turn: effect logic uses `delay` or `delay_hit`, so it needs a "
                          f"{{\"run\": \"delayed\"}} phase (where the pending effects land)")


def field_placeholders(scripts: str | Path | None, codepage: text_encoder.CodePage, where: str,
                       deps: list[Path] | None) -> dict[str, int] | None:
   """!The field placeholders a game's scripts manifest declares, for the battle's in-game texts (D-22 decision 6:
   battle::format_text() keeps their FD 80-FE codes for the printer's field resolver).
   @param scripts   the scripts manifest, or None
   @param codepage  the game's code page
   @param where     location
   @param deps      list collecting files read, or None
   @return {name: argument byte}, or None when there is no manifest or it declares none
   @throws ManifestError when the manifest is malformed or a name is one of the battle's own placeholders
   """
   if scripts is None:
      return None
   import script_compiler as sc
   try:
      fp = sc.load_placeholders(scripts, codepage)
   except sc.ScriptError as e:
      raise ManifestError(f"{where}: scripts {Path(scripts).name}: {e.message}") from None
   if deps is not None:
      deps.append(Path(scripts).resolve())
   if fp is None:
      return None
   clash = sorted(set(fp.codes) & set(PLACEHOLDERS))
   if clash:
      raise ManifestError(f"{where}: the field placeholder(s) {', '.join(clash)} of {Path(scripts).name} are also "
                          f"battle placeholders; rename them")
   return dict(fp.codes)


def main(argv: Sequence[str]) -> int:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return process exit status
   """
   args = list(argv[1:])
   try:
      codepage = text_encoder.option(args)
   except text_encoder.TextError as e:
      print(f"error: {e}", file=sys.stderr)
      return 1
   opts = {}
   for flag in ("--depfile", "--ids", "--platform", "--scripts"):
      if flag in args:
         i = args.index(flag)
         if i + 1 >= len(args):
            print(__doc__)
            return EXIT_USAGE
         opts[flag] = args[i + 1]
         del args[i:i + OPTION_WORDS]
   if len(args) != POSITIONAL_ARGS:
      print(__doc__)
      return EXIT_USAGE
   src, dst = Path(args[0]), Path(args[1])
   deps = []
   try:
      c = compile_battle(src, deps, codepage=codepage, platform=opts.get("--platform"), scripts=opts.get("--scripts"))
      ids = emit_ids(src.name, c) if "--ids" in opts else None
   except (ManifestError, KeyError, TypeError, ValueError) as e:
      print(f"{src}: error: {e}", file=sys.stderr)
      return 1
   write_if_changed(dst, emit(src.name, c))
   if ids is not None:
      write_if_changed(Path(opts["--ids"]), ids)
   if "--depfile" in opts:
      def esc(p: Path) -> str:
         """!A path as a Make depfile writes it (forward slashes, spaces escaped)."""
         return str(p).replace("\\", "/").replace(" ", "\\ ")
      write_if_changed(Path(opts["--depfile"]), f"{esc(dst)}: " + " ".join(esc(p) for p in sorted(set(deps))) + "\n")
   print(f"{src}: {len(c['types'])} types, {len(c['moves'])} moves, {len(c['species'])} species, "
         f"{len(c['parties'])} parties, {len(c['formats'])} formats, {len(c['stats'])} stats, "
         f"record {c['layout']['bytes']} B")
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv))
