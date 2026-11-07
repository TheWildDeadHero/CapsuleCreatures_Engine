#!/usr/bin/env python3
"""Snapshot of every generated source: runs each build-time compiler the way
CMakeLists.txt does (the engine's own inputs, tests/data and every app's
reference/project.json) and compares the sha256 of each output with
tests/data/snapshot/hashes.json.

A difference means a compiler changed what it writes. When the change is
intended, record it: list the outputs and the reason in RECORDED_CHANGES and
rewrite the hashes with

    python3 tests/test_generated_snapshot.py --record

Depfiles are not compared (they name absolute paths).
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
HASHES = ROOT / "tests" / "data" / "snapshot" / "hashes.json"

## Why the Unicode text work battle outputs changed (Unicode text through per-language code pages).
UNICODE_TEXT_BATTLE = ("Placeholders are FD nn (capsule::text::Arg) instead of the single codes F0-F8 (one byte more "
                       "per placeholder); kOffsets has one more entry (0xFFFF) for the appended text_language role")
## Why the generic parties work battle outputs changed (generic parties, traits, passives, categories,
## presenters, terms).
GENERIC_PARTIES_BATTLE = ("kData holds parties (PartyData with presenter slots, rank and flags) instead of trainers, "
                          "presenters with an embedded entrance sequence, move categories, format stands, the reward() "
                          "formula, rank_max and the confusion category; renamed tables (kPassives, kTraits, kMembers)")
## Why the generic parties work ids headers changed.
GENERIC_PARTIES_IDS = ("PASSIVE_, TRAIT_, PARTY_ ids (were ABILITY_, NATURE_, TRAINER_), new CATEGORY_ and PRESENTER_ "
                       "ids")
## Why the resources world outputs changed (field rules from the world manifest).
RESOURCES_WORLD = ("The world's elevation table (kElevations: meet mask, keep flag and sprite priority per level, "
                   "replacing kElevationPriority) and vehicle class table (kVehicleClasses: speeds, first animation, "
                   "can-run flag) are emitted and named by kWorld (vehicle classes last); every other table is "
                   "unchanged (a world without elevations compiles to the old rule)")
## Why the resources map test fixture's outputs changed further (the field rules' test data, tests/map_tests.cpp
## field_rules()).
RESOURCES_FIXTURE = ("The test world's custom levels (0 and 15 skip 12, 9 keeps and meets 5) and surf class (walk 2, "
                     "run 2, first animation 12, cannot run); map small paints levels 12, 9, 5, 6 and a water cell in "
                     "its top rows and forbids running (kMapNoRun)")
## Why the resources script outputs changed (the Vehicle opcode, `vehicle NAME;`).
RESOURCES_SCRIPTS = "New test statements `vehicle surf; vehicle 0;` (Vehicle opcode 44, class byte)"
## Why the resources map test scripts changed.
RESOURCES_MAP_SCRIPTS = "New test scripts ride_surf and walk_on (`vehicle surf;`, `vehicle 0;`), SCRIPT_ ids 16 and 17"
## Why every field placeholders script bank changed (field text placeholders).
PLACEHOLDERS_SCRIPTS = ("capsule::script::Bank gains the field placeholder table (placeholder_count in the former "
                        "reserved byte, then placeholders, names, name_bytes): a bank without placeholders writes one "
                        "more line `nullptr, nullptr, 0,`; every text keeps its bytes")
## Why the field placeholders test fixture's script outputs changed further.
PLACEHOLDERS_FIXTURE = ("tests/data/scripts declares placeholders hero (name slot 5), count (v16), town (term) and "
                        "lead (nickname of party slot 2) and the script greet ({prompt}, {page}, {@town:many}): the "
                        "kPlaceholders table, g_names[6], the term texts in kText, SCRIPT_GREET, PLACEHOLDER_ ids and "
                        "kScriptSaveBytes + 6")
## Why the field placeholders map test world's outputs changed (location names as field text through the world's
## scripts).
PLACEHOLDERS_MAP_FIXTURE = ("tests/data/maps/scripts declares the name slot {hero} (6) and the garden's location name "
                            "is \"{hero} Garden\" (FD 80 + \" Garden\"): kLocationName1, kPlaceholders, g_names[7], "
                            "PLACEHOLDER_HERO and kScriptSaveBytes + 7")
## Why the field placeholders map sandbox's script outputs changed further.
PLACEHOLDERS_SANDBOX = ("Field placeholders demo: the map sandbox declares {player} (name slot 7) and {berries} "
                        "(variable berries); the welcome message shows {player} and a {page}, the hidden berry counts "
                        "first and shows {player}, a {prompt} and {berries}: kPlaceholders, g_names[8], PLACEHOLDER_ "
                        "ids and kScriptSaveBytes + 8")
## Why the manifest stats battle outputs changed (C: stats from the manifest).
MANIFEST_STATS_BATTLE = ("The stats come from the manifest: new kStats (StatInfo: label, stage, flags) and kStatCount "
                         "with its kMaxStats static_assert; kStageNames (accuracy, evasion, critical, then the staged "
                         "stats' labels) replaces kStatNames; stat_stage components use the new stage indices "
                         "(accuracy 0, evasion 1, critical 2, staged stats from 3); trait percentages cover every stat "
                         "(100 for HP); category flags add CATEGORY_ON_MODIFY_ATK / _SPA; kData ends with stat_count, "
                         "pool_stat, order_stat, stage_count; kLayout gains kWidths (per-role element widths), "
                         "stat_count and a 16-bit record_bytes (hash and 60 bytes unchanged); stat_hp / stat_other "
                         "become file-local stat formulas called by rules::stat(); text symbols are renumbered (the "
                         "stat labels are defined first)")
## Why the manifest stats ids headers changed.
MANIFEST_STATS_IDS = "STAT_<NAME> ids and NUM_STATS (the manifest's stats)"
## Why the ruleset pipelines battle outputs changed (D: pipelines and points).
PIPELINES_BATTLE = ("The ruleset's pipelines (capsule.ruleset/2) are emitted as rules::damage / self_hit / speed / "
                    "accuracy / crit with the kPoint<Name> ids, damage_base becomes the file-local named formula "
                    "formula_damage_base, the point handlers are generated pure effect code (rules::point_handler); "
                    "kPoints (PointData) is new and kData names it with point_count; Rules lose crit_q8, stab_q8, "
                    "spread_q8, random_min_pct, crit_ignores_stages and the damage steps; StatusData loses speed_q8 / "
                    "physical_q8 and WeatherData its boost / weaken fields for first_handler, handler_count and "
                    "point_mask; the legacy modify handlers are point handlers (hook kPointBase + point, no "
                    "components: the component pool and the hook numbers shift; hooks lose OnModifyAtk / SpA / Spe, "
                    "OnBasePower and OnModifyDamage); categories lose the interim CATEGORY_ON_MODIFY_* flags")
## Why the ruleset pipelines ids headers changed.
PIPELINES_IDS = "POINT_<NAME> ids and NUM_POINTS (the ruleset's points)"
## Why the layout roles battle outputs changed (A8-A10, : roles and layout v2).
LAYOUT_ROLES_BATTLE = ("kLayout gains kCounts (per-role element counts), the custom role table kCustom (RoleSlot: name "
                       "hash, default, offset, width, count; owner_secret, friendship and nicknamed are custom roles "
                       "now at the bit offsets they had, so their kOffsets entries are 0xFFFF) and move_slots, "
                       "party_size, custom_count; kParties' MemberData moves hold kMaxMoveSlots (8) entries, padded "
                       "with 0xFFFF; hash 0x62409274 and 60 record bytes unchanged")
## Why the layout roles ids headers changed.
LAYOUT_ROLES_IDS = "ROLE_<NAME> ids of the layout's roles (engine IDs, custom roles 0x80 + index) and NUM_CUSTOM_ROLES"
## Why the layout roles test fixture's script outputs changed (role_get / role_set, record bindings of custom roles).
LAYOUT_ROLES_SCRIPTS = ("tests/data/scripts names the battle_stats3 layout (\"battle\"), declares {met} (met_level of "
                        "party slot 1) and the script records (RoleGet 45 / RoleSet 46 on custom and engine roles); "
                        "Placeholder::text of a record binding is its text codes (lead: 8, the layout's nickname): "
                        "kCode, kPlaceholders, the var met, SCRIPT_RECORDS, VAR_MET, PLACEHOLDER_MET, ROLE_ ids and "
                        "kScriptSaveBytes + 1")
## Why the effect bank battle outputs changed (the effect VM runs the battle's logic).
EFFECT_BANK_BATTLE = ("The effect bank (kEffectCode, kEffectPrograms, kEffectHandlers, kEffectOwners: one owner per "
                      "move, passive, item, status and weather, the legacy effect lists lowered to effect code, point "
                      "handlers joining their owner) replaces kComponents, kHandlers and EffectData; MoveData names "
                      "its owner and the AI's summary (fixed damage, statuses, formula); HolderData {name, owner} for "
                      "passives and items; StatusData / WeatherData name their owner instead of first_handler, "
                      "handler_count and point_mask; kTexts (owner texts) and text_count; the protection move flag on "
                      "the move whose first effect starts protection")
## Why the effect bank effect banks changed (no fixed point enum; the SayAt opcode).
EFFECT_BANK_EFFECTS = ("The static_asserts lose effect::Point (points are the ruleset's) and gain the SayAt opcode; "
                       "the VM fixture's Everything program says `say generic at user about target value 3;`")
## Why the condition records battle outputs changed (A3-A5, effect logic sections 1.5 and 2: conditions as manifest
## records).
CONDITIONS_BATTLE = ("Conditions are records: StatusData {texts, owner, color} and WeatherData {texts, owner} lose "
                     "their declarative fields (they expand into effect programs: try_start / before_move / "
                     "damaging_hit / residual / switch_out / field_residual handlers in the bank), new kVolatiles, "
                     "kSideConds, kFieldConds tables and their counts plus status_bits in kData; Rules {stat_cap, "
                     "fallback_move, level_max, rank_max, self_hit_power, self_hit_category} replaces the confusion / "
                     "protect fields; MoveData::flags is 16 bits after fixed_fraction (manifest move_flags indices); "
                     "owners for volatiles (side and field conditions: none in the data); the Cure opcode takes a "
                     "flags byte (cure ... quiet); `weather NAME;` pushes the weather's own turns; the sample's Daze "
                     "Ray / Guard logic starts the confusion / protect_chain / protect volatiles, whose records and "
                     "code hold the rest (same events and draws: goldens unchanged)")
## Why the condition records ids headers changed.
CONDITIONS_IDS = "VOLATILE_, SIDE_COND_, FIELD_COND_, FLAG_ (move flags) and TAG_ ids with their NUM_ counts"
## Why the condition records effect banks changed (the Cure opcode's flags byte; the new try_start hook in the
## static_asserts).
CONDITIONS_EFFECTS = ("Cure is `Cure subj8 flags8` (kCureQuiet) and the hook list ends with try_start: the cure "
                      "statements and the code after them move by one byte")
## Why the tier records battle outputs changed (chart tiers and stages from
## the manifest).
TIERS_BATTLE = ("The type chart's tiers are records (kTiers: TierData {q8, exp, kind, message}; kChart holds tier "
                "indices, the sample's seven tiers in the old Tier code order so its kChart bytes are unchanged) and "
                "the stages are records (kStages: StageData {name, table, odds, lo, hi, kind, stat, formula} with "
                "their tables, equal tables emitted once) replacing kStageNames and Rules' stat_stage / accuracy_stage "
                "/ crit_odds; SpeciesData::types holds kMaxTypesPer (3) entries; kData names kTiers and kStages and "
                "ends with tier_count, combine, species_types, added_types, above / below messages and the accuracy, "
                "evasion, crit and shared stat stage indices; rules::stage_formula() (formula stages) is generated; "
                "pipelines, effect bytecode and every other table unchanged")
## Why the tier records ids headers changed.
TIERS_IDS = "STAGE_<NAME> ids (the stages in manifest order) and TIER_<NAME> ids with NUM_STAGES / NUM_TIERS"
## Why the turn rules battle outputs changed (A11: the turn structure from the manifest).
TURN_RULES_BATTLE = ("Rules ends with TurnRules (brackets run / switch / item / move with move_priority, tie rule, "
                     "send-out order, start switch-in order, effect_order, end_count and eight EndPhaseData {phase, "
                     "each, source_count, sources}); a data set without `turn` gets the engine's former fixed order "
                     "(run 30, switch 20, move priority, a random tiebreak byte, end of turn weather / battlers / "
                     "sides / field, dispatch status, volatile, side, field, weather, passive, item); every other "
                     "table unchanged")
## Why the generation entries battle outputs changed (the game-tables stage / A12: progression from the manifest).
GENERATION_BATTLE = ("kGeneration (GenData {value, span, role, kind, count, derived} per generation entry) and Rules "
                     "ends with ProgressRules {kGeneration, count, learn on_full, default_moves, exp share, receivers, "
                     "outsider}; the ruleset's pp_max() slot and rules::derive() are emitted; a data set without learn "
                     "/ exp_policy / generation / pp_max gets the engine's former fixed rules (skip a full move list, "
                     "the last moves by level, faced battlers of the human side without outsiders, draws seed / genes "
                     "/ trait / passive slot, a fifth of the base PP per raise); every other table unchanged")
## Why the stage scaling battle outputs changed (the data sets' rulesets name each stat's own stage and the
## chart's combine mode instead of the shared stage table and the exponent).
STAGE_SCALING_BATTLE = ("The rulesets' damage, self_hit and speed pipelines scale each stat by its own stage "
                        "(battle::pipeline_stage_scale(in.*_stage_id, ...) instead of battle::pipeline_staged()) and "
                        "damage by the type chart (battle::pipeline_type_scale(ctx, l_d) instead of rules::pow2(l_d, "
                        "in.type_exp)); the same values (goldens unchanged), every table unchanged")
## Why the stage scaling sample outputs changed further.
STAGE_SCALING_SAMPLE = STAGE_SCALING_BATTLE + ("; the sample's decisive ko tier (used by no matchup) names no message: "
                                               "kTiers' ko entry message 255 instead of super_effective's 8")
## Why the stage scaling transfer tables are new.
STAGE_SCALING_TRANSFER = "capsule_game() compiles each game's record transfer table (kTransfer) next to its battle data"
## The battle outputs' history (every battle data set changed alike).
BATTLE_HISTORY = "; then ".join([UNICODE_TEXT_BATTLE, GENERIC_PARTIES_BATTLE, MANIFEST_STATS_BATTLE, PIPELINES_BATTLE,
                                 LAYOUT_ROLES_BATTLE, EFFECT_BANK_BATTLE, CONDITIONS_BATTLE,
                                 TIERS_BATTLE, TURN_RULES_BATTLE, GENERATION_BATTLE, STAGE_SCALING_SAMPLE])
## The tier records history of the neutral fixtures (their chart: one x1 tier, combine multiply).
TIERS_FIXTURES = TIERS_BATTLE.replace("the sample's seven tiers in the old Tier code order so its kChart bytes are "
                                      "unchanged", "the neutral fixtures declare one x1 tier")
## Outputs whose recorded hash was changed on purpose, with the reason (newest last). Every other output of the
## snapshot taken from an older version kept its hash; the Unicode text work also added test_text/font.cpp (the text
## test app's code page). The generic parties work's spotter rename (map objects) writes the same tables, so the map
## outputs kept their hashes.
RECORDED_CHANGES = {
                    "test_battle.cpp": BATTLE_HISTORY,
                    "test_battle/battle.cpp": BATTLE_HISTORY,
                    "sandbox_battle/battle.cpp": BATTLE_HISTORY,
                    "battle_ids.h": (GENERIC_PARTIES_IDS + "; then " + MANIFEST_STATS_IDS + "; then " + PIPELINES_IDS
                                     + "; then "
                    + LAYOUT_ROLES_IDS + "; then " + CONDITIONS_IDS
                                     + "; then " + TIERS_IDS),
                    "test_battle/battle_ids.h": (GENERIC_PARTIES_IDS + "; then " + MANIFEST_STATS_IDS + "; then "
                                                 + PIPELINES_IDS
                                                 + "; then " + LAYOUT_ROLES_IDS + "; then "
                                + CONDITIONS_IDS + "; then " + TIERS_IDS),
                    "sandbox_battle/battle_ids.h": (GENERIC_PARTIES_IDS + "; then " + MANIFEST_STATS_IDS + "; then "
                                                    + PIPELINES_IDS
                                                    + "; then " + LAYOUT_ROLES_IDS +
                                                    "; then " + CONDITIONS_IDS + "; then " + TIERS_IDS),
                    "battle_stats3/test_battle.cpp": (CONDITIONS_BATTLE + " (the fixture declares move_flags contact "
                                                      "and protect); then "
                                     + TIERS_FIXTURES + "; then " + TURN_RULES_BATTLE + "; then " + GENERATION_BATTLE
                                                      + "; then "
                                     + STAGE_SCALING_BATTLE),
                    "battle_stats3/battle_ids.h": CONDITIONS_IDS + "; then " + TIERS_IDS,
                    "battle_slots2/test_battle.cpp": (CONDITIONS_BATTLE + " (the fixture declares move_flags contact "
                                                      "and protect); then "
                                     + TIERS_FIXTURES + "; then " + TURN_RULES_BATTLE + "; then " + GENERATION_BATTLE
                                                      + "; then "
                                     + STAGE_SCALING_BATTLE),
                    "battle_slots2/battle_ids.h": CONDITIONS_IDS + "; then " + TIERS_IDS,
                    "battle_vm/test_battle.cpp": (CONDITIONS_BATTLE + " (the VM fixture declares move_flags contact "
                                                  "and protect); then "
                                 + TIERS_FIXTURES + "; then " + TURN_RULES_BATTLE + "; then " + GENERATION_BATTLE
                                                  + "; then "
                                 + STAGE_SCALING_BATTLE),
                    "battle_vm/battle_ids.h": CONDITIONS_IDS + "; then " + TIERS_IDS,
                    "battle_conditions/test_battle.cpp": (TIERS_FIXTURES + "; then " + TURN_RULES_BATTLE + "; then "
                                         + GENERATION_BATTLE + "; then "
                                         + STAGE_SCALING_BATTLE + " (the fixture shares the VM fixture's ruleset)"),
                    "battle_tiers/test_battle.cpp": TURN_RULES_BATTLE + "; then " + GENERATION_BATTLE,
                    "battle_turn/test_battle.cpp": GENERATION_BATTLE + "; then " + STAGE_SCALING_BATTLE
                    + " (the fixture shares the VM fixture's ruleset)",
                    "battle_progress/test_battle.cpp": STAGE_SCALING_BATTLE,
                    "test_battle/transfer.cpp": STAGE_SCALING_TRANSFER,
                    "sandbox_battle/transfer.cpp": STAGE_SCALING_TRANSFER,
                    "battle_conditions/battle_ids.h": TIERS_IDS,
                    "sandbox_battle/assets.cpp": "The sandbox's sprite:PRESENTER (a simple-shape presenter, 32x64)",
                    "sandbox_map/world.cpp": RESOURCES_WORLD
                    + " (the sandbox writes its levels out: the old rule, level 5 at priority 1; its surf class walks "
                    "2 pixels a frame and cannot run)",
                    "test_world.cpp": RESOURCES_WORLD + "; " + RESOURCES_FIXTURE + "; then " + PLACEHOLDERS_MAP_FIXTURE,
                    "test_script.cpp": RESOURCES_SCRIPTS + "; then " + PLACEHOLDERS_SCRIPTS + "; "
                    + PLACEHOLDERS_FIXTURE + "; then "
   + LAYOUT_ROLES_SCRIPTS,
                    "test_script_ids.h": PLACEHOLDERS_FIXTURE + "; then " + LAYOUT_ROLES_SCRIPTS,
                    "test_map_scripts.cpp": RESOURCES_MAP_SCRIPTS + "; then " + PLACEHOLDERS_SCRIPTS + "; "
                    + PLACEHOLDERS_MAP_FIXTURE,
                    "test_map_script_ids.h": RESOURCES_MAP_SCRIPTS + "; then " + PLACEHOLDERS_MAP_FIXTURE,
                    "test_script/scripts.cpp": PLACEHOLDERS_SCRIPTS,
                    "sandbox_map/scripts.cpp": PLACEHOLDERS_SCRIPTS + "; " + PLACEHOLDERS_SANDBOX,
                    "sandbox_map/script_ids.h": PLACEHOLDERS_SANDBOX,
                    "test_effects_vm.cpp": EFFECT_BANK_EFFECTS + "; then " + CONDITIONS_EFFECTS,
                    "test_effects_classic.cpp": EFFECT_BANK_EFFECTS + "; then " + CONDITIONS_EFFECTS,
}

## Why every resources battle output changed (resources are manifest records; PP is one of them).
RESOURCES_BATTLE = ("kResources (ResourceData {label, regen, scope, empty, slot, stat, role, raises, max_formula, "
                    "regen_formula} per manifest resource: the hit points on the pool stat, and pp, the move resource "
                    "with base pp, max pp_max, raises pp_ups, persisted in pp) and kMoveCosts (MoveCost {amount, "
                    "resource, formula}: 1 PP per move); MoveData has cost_first / cost_count / base[] instead of pp; "
                    "kData ends with resource_count, faint_resource, pool_count, move_resource_count after the stage "
                    "fields and names kResources / kMoveCosts after kTexts; the ruleset's pp_max slot is a file-local "
                    "named formula called by rules::resource_formula(); the message no_pp is no_usable_move (same "
                    "text); every other table unchanged (goldens unchanged)")
## Why the resources fixtures without a ruleset pp_max changed further.
RESOURCES_DEFAULT_PP_MAX = RESOURCES_BATTLE + (" (the fixture's ruleset states the former default pp_max as a named "
                                               "formula, pp_base + pp_base * pp_ups / 5)")
## Why the resources fixture without PP raises changed further.
RESOURCES_NO_RAISES = RESOURCES_BATTLE.replace("max pp_max, raises pp_ups",
                                               "no max formula (the base: the layout holds no "
                                               "raises)") + (" (no resource formula: rules::resource_formula() returns "
                                                             "its base)")
## Why the resources ids headers changed.
RESOURCES_IDS = "RESOURCE_<NAME> ids and NUM_RESOURCES (the manifest's resources)"
## Why the resources effect banks changed (the opcode and ref tables they assert).
RESOURCES_EFFECTS = ("The enum static_asserts name the appended opcodes Restore and Raise and the refs MoveBase (was "
                     "MovePp), Resource, ResourceMax and MoveCost")
## The resources changes, appended to each output's history.
RESOURCES_CHANGES = {
   **{name: RESOURCES_BATTLE for name in ("test_battle.cpp", "test_battle/battle.cpp", "sandbox_battle/battle.cpp",
                                          "battle_progress/test_battle.cpp")},
   **{name: RESOURCES_DEFAULT_PP_MAX for name in ("battle_stats3/test_battle.cpp", "battle_vm/test_battle.cpp",
                                                  "battle_turn/test_battle.cpp", "battle_conditions/test_battle.cpp")},
   **{name: RESOURCES_NO_RAISES for name in ("battle_slots2/test_battle.cpp", "battle_tiers/test_battle.cpp")},
   **{name: RESOURCES_IDS for name in ("battle_ids.h", "test_battle/battle_ids.h", "sandbox_battle/battle_ids.h",
                                       "battle_stats3/battle_ids.h", "battle_slots2/battle_ids.h",
                                       "battle_vm/battle_ids.h",
                                       "battle_conditions/battle_ids.h", "battle_tiers/battle_ids.h",
                                       "battle_turn/battle_ids.h", "battle_progress/battle_ids.h")},
                     "test_effects_vm.cpp": RESOURCES_EFFECTS + "; the VM fixture's Everything move restores and "
                     "raises resources",
                     "test_effects_classic.cpp": RESOURCES_EFFECTS,
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in RESOURCES_CHANGES.items()})

## Why every battle output changed in the field placeholders (effect odds: the odds stat and the optional effect_odds
## pipeline).
PLACEHOLDERS_BATTLE = ("kData ends with odds_stat (the stats[].odds stat, 255 without one) and the ruleset's "
                       "rules::effect_odds() pipeline is emitted (a ruleset without one gets the plain odds: chance = "
                       "effect_chance, out_of = effect_out_of, a roll unless certain); every table unchanged")
## Why the field placeholders effect banks changed (the opcode table they assert).
PLACEHOLDERS_EFFECTS = "The enum static_asserts name the appended opcode Odds (effect odds, 4 operands)"
## The field placeholders changes, appended to each output's history.
PLACEHOLDERS_CHANGES = {
   **{name: PLACEHOLDERS_BATTLE for name in ("test_battle.cpp", "test_battle/battle.cpp", "sandbox_battle/battle.cpp",
                                             "battle_stats3/test_battle.cpp", "battle_slots2/test_battle.cpp",
                                             "battle_vm/test_battle.cpp", "battle_conditions/test_battle.cpp",
                                             "battle_tiers/test_battle.cpp", "battle_turn/test_battle.cpp",
                                             "battle_resources/test_battle.cpp")},
                        "battle_progress/test_battle.cpp": PLACEHOLDERS_BATTLE + "; the fixture's terms and "
                        "escape_blocked text are neutral (Rival, Temperament, Talent, Coins; \"No running from this "
                        "battle!\")",
                        "test_effects_vm.cpp": PLACEHOLDERS_EFFECTS + "; the VM fixture's global \"odds\" owner uses "
                        "it",
                        "test_effects_classic.cpp": PLACEHOLDERS_EFFECTS,
   **{name: "New, the Starter sample's battle data (samples/starter; the starter_golden_tests data set)"
      for name in ("starter/test_battle.cpp", "starter/battle_ids.h")},
   **{name: "New, the Starter sample's game apps/starter_battle (samples/starter's battle data and shape art)"
      for name in ("starter_battle/assets.cpp", "starter_battle/battle.cpp", "starter_battle/battle_ids.h",
                   "starter_battle/transfer.cpp")},
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in PLACEHOLDERS_CHANGES.items()})

## Why the map outputs changed in a review (neutral names in the map sample data; no table layout changes).
NEUTRAL_NAMES_SANDBOX = ("Neutral names in the map sandbox: vehicle class and encounter method surf -> swim, object "
                         "trainer -> rival, sprite:TRAINER -> sprite:RIVAL (a new asset key hash), flag beat_trainer "
                         "and scripts trainer_battle / trainer_after -> beat_rival / rival_battle / rival_after")
## Why the map test fixture's outputs changed in a review.
NEUTRAL_NAMES_FIXTURE = ("Neutral names in tests/data/maps: vehicle class and encounter method surf -> swim, object "
                         "trainer -> rival, flag beat_trainer and scripts trainer_battle / ride_surf -> beat_rival / "
                         "rival_battle / ride_swim (same IDs and values)")
## The review changes, appended to each output's history.
NEUTRAL_NAMES_CHANGES = {
   **{name: NEUTRAL_NAMES_SANDBOX for name in ("sandbox_map/assets.cpp", "sandbox_map/encounter_ids.h",
                                               "sandbox_map/script_ids.h", "sandbox_map/scripts.cpp",
                                               "sandbox_map/world.cpp",
                                               "sandbox_map/world_ids.h")},
   **{name: NEUTRAL_NAMES_FIXTURE for name in ("test_encounter_ids.h", "test_map_script_ids.h", "test_world.cpp",
                                               "test_world_ids.h")},
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in NEUTRAL_NAMES_CHANGES.items()})

## The classic sample and its outputs left for the reference-ruleset repo (tests/data/battle, the sandbox_battle
## app, the classic effect examples): their earlier reasons go with them.
STARTER_SAMPLE_GONE = ("battle_ids.h", "test_battle.cpp", "test_effects_classic.cpp")
for _name in [n for n in RECORDED_CHANGES if n in STARTER_SAMPLE_GONE or n.startswith("sandbox_battle/")]:
   del RECORDED_CHANGES[_name]
## Starter sample reasons of the outputs that changed.
STARTER_SAMPLE_DOTS = ("The asset compiler's dot patterns draw from a neutral LCG (the reference ruleset's Method-1 "
                       "constants left the engine), so dotted shapes move")
STARTER_SAMPLE_STARTER = ("The Starter sample's Vine Lash heals dealt / 4 (was dealt * 64 >> 8, which could saturate; "
                          "the same values)")
STARTER_SAMPLE_MAPS = ("The map fixture and the map sandbox name the Starter sample's battle data and creatures "
                       "(encounters: Sproutle, Duskowl, Voltmink, Tidepup, Clodbun, Breezle, Glimmet)")
STARTER_SAMPLE_TEST_BATTLE = "The battle test app runs on the Starter sample (samples/starter)"
STARTER_SAMPLE_TRANSFER = ("The transfer tests use the Starter sample's table and its twin fixture "
                           "(tests/data/transfer/twin.json, case sample_to_twin)")
STARTER_SAMPLE_CHANGES = {
   **{name: STARTER_SAMPLE_STARTER for name in ("starter/test_battle.cpp", "starter_battle/battle.cpp")},
                          "test_world.cpp": STARTER_SAMPLE_MAPS,
                          "sandbox_map/world.cpp": STARTER_SAMPLE_MAPS + "; " + STARTER_SAMPLE_DOTS + " (the cave and "
                          "outdoor tilesets)",
                          "sandbox_sequence/assets.cpp": STARTER_SAMPLE_DOTS,
   **{name: STARTER_SAMPLE_TEST_BATTLE for name in ("test_battle/battle.cpp", "test_battle/battle_ids.h",
                                                    "test_battle/transfer.cpp")},
   **{name: STARTER_SAMPLE_TRANSFER for name in ("transfer/test_transfer.cpp", "transfer/transfer_cases.h")},
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in STARTER_SAMPLE_CHANGES.items()})

## Why every battle output changed in the move costs (the engine review's fixes).
MOVE_COSTS_BATTLE = ("MoveData::power is 16 bits (after cost_first) and SpeciesData::base 16 bits per stat; kData "
                     "drops pool_count and ends with passive_slots, passive_rolled and passive_selector_mask (the "
                     "manifest's passive slots); optional messages a manifest leaves out are nullptr; static_asserts "
                     "tie the categories, resources, pools, stage ranges and passive slots to the engine's caps; the "
                     "effect bank's weather event budget counts its try_start hook")
## The fixtures' own move costs changes, after MOVE_COSTS_BATTLE.
MOVE_COSTS_TERMS = "the fixture's terms and escape text are neutral (Rival, Temperament, Talent, Coins)"
MOVE_COSTS_CHANGES = {
   **{name: MOVE_COSTS_BATTLE + "; " + MOVE_COSTS_TERMS for name in ("battle_conditions/test_battle.cpp",
                                                                     "battle_slots2/test_battle.cpp",
                                                                     "battle_stats3/test_battle.cpp",
                                                                     "battle_tiers/test_battle.cpp",
                                                                     "battle_vm/test_battle.cpp")},
                      "battle_presenters/test_battle.cpp": MOVE_COSTS_BATTLE
                      + "; Burst costs 80 MP (the most a pool reaches: a cost no pool "
                      "reaches is refused); " + MOVE_COSTS_TERMS,
                      "battle_progress/test_battle.cpp": MOVE_COSTS_BATTLE + "; the pp role is 8 bits (pp_max reaches "
                      "135: a persist role holds its pool's maximum)",
                      "battle_resources/test_battle.cpp": MOVE_COSTS_BATTLE + "; Burst costs 80 MP; the move Sacrifice "
                      "(5 hit points); "
                                       + MOVE_COSTS_TERMS,
                      "battle_resources/battle_ids.h": "The move Sacrifice (MOVE_SACRIFICE)",
                      "battle_turn/test_battle.cpp": MOVE_COSTS_BATTLE + "; the weather Gale with its own hooks, the "
                      "volatile breeze, the "
                      "move Gale and the party Gusty (the weather lifecycle test); " + MOVE_COSTS_TERMS,
                      "battle_turn/battle_ids.h": "WEATHER_GALE, VOLATILE_BREEZE, MOVE_GALE, PARTY_GUSTY and its texts",
   **{name: MOVE_COSTS_BATTLE
      + "; the Starter leaves out the optional messages hit_count, protected, protect_start and recoil, and declares "
      "two rolled passive slots"
      for name in ("starter/test_battle.cpp", "starter_battle/battle.cpp", "test_battle/battle.cpp")},
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in MOVE_COSTS_CHANGES.items()})

## Why the Starter's outputs changed in the Starter's rules (the developer's decision of 2026-10-19: the Starter has its
## own numbers and formulas, README "Starter rules"; no table layout changes).
STARTER_RULES_STARTER = ("The Starter's own rules: stat formulas with a head start and a linear growth, damage Attack "
                         "x power x (level + 5) / ((Defence + 10) x 50) + 1 with a 92..108 % roll after the chart, "
                         "crit x1.625 piercing defence stages, same type x1.2, spread x0.7, stages -4..+4 (+25 % / -15 "
                         "% a stage, accuracy 10 %), crit odds 1/20..3/10, tiers x0.6 / x1.75, temperaments 115 / 88, "
                         "level 50, stat cap 700, self-hit 30, quadratic growth brisk / steady, its own exp, reward, "
                         "escape and EP regen formulas, the odds (2 x user + 60) / (user + target + 60), its own "
                         "status, weather and talent factors, a neutral appeared text and save_resources on Scholar "
                         "Wen")
STARTER_RULES_CHANGES = {name: STARTER_RULES_STARTER for name in ("starter/test_battle.cpp",
                                                                  "starter_battle/battle.cpp",
                                                                  "test_battle/battle.cpp")}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in STARTER_RULES_CHANGES.items()})

## Why the neutral fixtures' outputs changed in the neutral fixture (no classic shapes left in the engine's neutral
## battle data; no table layout changes).
NEUTRAL_FIXTURE_FIXTURES = ("The neutral fixture takes the Starter's numbers instead of classic-shaped ones: stages "
                            "-4..+4 (+25 % / -15 % a stage, accuracy 10 %), crit odds 1/20..3/10, level 50, stat cap "
                            "700, self-hit 30, the Starter's stat, damage, reward and escape formulas and damage "
                            "pipeline, temperaments 115 / 88, growth 40n, and the neutral texts \"{user} blocks the "
                            "way!\" and dazed")
NEUTRAL_FIXTURE_CHANGES = {
   **{name: NEUTRAL_FIXTURE_FIXTURES for name in ("battle_stats3/test_battle.cpp", "battle_slots2/test_battle.cpp",
                                                  "battle_vm/test_battle.cpp")},
                           "battle_progress/test_battle.cpp": NEUTRAL_FIXTURE_FIXTURES + "; growth steady 8n^2 + 32n",
                           "battle_resources/test_battle.cpp": NEUTRAL_FIXTURE_FIXTURES
                           + "; growth steady 8n^2 + 32n; Burst costs 66 MP (the most a pool reaches now)",
                           "battle_presenters/test_battle.cpp": NEUTRAL_FIXTURE_FIXTURES + "; growth steady 8n^2 + "
                           "32n; Burst costs 66 MP",
                           "battle_turn/test_battle.cpp": NEUTRAL_FIXTURE_FIXTURES + "; Blight 1/10 and Dust 1/12 a "
                           "turn",
                           "battle_conditions/test_battle.cpp": NEUTRAL_FIXTURE_FIXTURES
                           + "; Blight 1/10 a turn; Thorns 1/10, 1/8, 1/6; Retort x1.5",
                           "battle_tiers/test_battle.cpp": ("Tiers x0.625 / x1.75 (sequential, still not powers of "
                                                            "two), crit odds 1/12, 1/6, 1/3, level 50, stat cap 700, "
                                                            "self-hit 30, the Starter's stat, damage, reward and "
                                                            "escape formulas and damage pipeline, temperaments 115 / "
                                                            "88, growth 40n, neutral texts"),
                           "test_effects_vm.cpp": "self_hit 30 in the opcode fixture (vm_ops.ceffect)",
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in NEUTRAL_FIXTURE_CHANGES.items()})

## Why the battle outputs changed in the battle messages (spot review 2's fixes): BattleData ends with message_count
## (the engine's Msg ids, then a battle's own messages) and the kMessages size check allows a battle's own messages.
MESSAGES_BATTLE = ("BattleData::message_count (the engine's message ids, then the battle's own) and kMessages checked "
                   "against Msg::NUM_MSGS as a lower bound")
MESSAGES_CHANGES = {
   **{name: MESSAGES_BATTLE for name in ("battle_conditions/test_battle.cpp", "battle_presenters/test_battle.cpp",
                                         "battle_progress/test_battle.cpp", "battle_resources/test_battle.cpp",
                                         "battle_slots2/test_battle.cpp", "battle_stats3/test_battle.cpp",
                                         "battle_tiers/test_battle.cpp", "battle_vm/test_battle.cpp")},
                    "battle_turn/test_battle.cpp": MESSAGES_BATTLE + "; Gale's end hook reads field.weather and Dust "
                    "has an end hook (the old weather's end hook acts on itself)",
   **{name: MESSAGES_BATTLE + "; the Starter's own message keys dazed, daze_hit, daze_end, became_dazed, already_dazed "
      "and staggered (\"{user} staggered!\") instead of confused / flinched, and its staged stats up to STAGED_CAP "
      "1400 (the stat cap x the top stage's x2) instead of the stat cap"
      for name in ("starter/test_battle.cpp", "starter_battle/battle.cpp", "test_battle/battle.cpp")},
                    "battle_passives/battle_ids.h": "The passive slot fixture "
                    "(tests/data/battle_progress/passives.json), new",
                    "battle_passives/test_battle.cpp": "The passive slot fixture "
                    "(tests/data/battle_progress/passives.json), new",
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in MESSAGES_CHANGES.items()})

## Why every battle and transfer fixture output changed in the record fields: capsule::monster::Layout holds
## unstored_count (the unstored custom roles after the stored ones, in the former padding), so every kLayout initializer
## ends
## `custom_count, unstored_count, {0}}` instead of `custom_count, {0, 0}}`; no table changes otherwise.
RECORD_FIELDS_LAYOUT = ("kLayout's initializer gains Layout::unstored_count (0 here) in the former padding: `..., "
                        "custom_count, 0, {0}};` instead of `..., custom_count, {0, 0}};`")
RECORD_FIELDS_CHANGES = {
   **{name: RECORD_FIELDS_LAYOUT for name in ("battle_conditions/test_battle.cpp", "battle_presenters/test_battle.cpp",
                                              "battle_progress/test_battle.cpp", "battle_resources/test_battle.cpp",
                                              "battle_slots2/test_battle.cpp", "battle_stats3/test_battle.cpp",
                                              "battle_tiers/test_battle.cpp", "battle_vm/test_battle.cpp",
                                              "battle_turn/test_battle.cpp", "battle_passives/test_battle.cpp",
                                              "starter/test_battle.cpp", "starter_battle/battle.cpp",
                                              "test_battle/battle.cpp",
                                              "transfer/transfer_four.cpp", "transfer/transfer_shuffled.cpp",
                                              "transfer/transfer_six.cpp", "transfer/transfer_twin.cpp")},
                         "battle_records/battle_ids.h": "The records fixture "
                         "(tests/data/battle_progress/records.json), new",
                         "battle_records/test_battle.cpp": ("The records fixture "
                                                            "(tests/data/battle_progress/records.json), new: a "
                                                            "low-first wide seed (GenKind 5), an unstored custom role "
                                                            "(offset 0xFFFF, Layout::unstored_count 1), kSpeciesFields "
                                                            "and species_field() in derive()"),
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in RECORD_FIELDS_CHANGES.items()})

## Why every battle output changed in the effect handlers: Rules gains move_type_point after self_hit_category (the
## ruleset's
## `move_type` point, kNone8 without one), the PointSource static_assert counts the five new sources (move, user /
## target side, user / target volatile), and rules::species_field_of() is generated (0 without a species field read
## by logic; the record fields' table is kept for derived expressions).
EFFECT_HANDLERS_BATTLE = ("Rules' initializer gains move_type_point (255 here) after self_hit_category; PointSource "
                          "has 12 sources (static_assert); rules::species_field_of() (returns 0 here)")
EFFECT_HANDLERS_CHANGES = {
   **{name: EFFECT_HANDLERS_BATTLE for name in ("battle_conditions/test_battle.cpp",
                                                "battle_presenters/test_battle.cpp",
                                                "battle_progress/test_battle.cpp", "battle_resources/test_battle.cpp",
                                                "battle_slots2/test_battle.cpp", "battle_stats3/test_battle.cpp",
                                                "battle_tiers/test_battle.cpp", "battle_vm/test_battle.cpp",
                                                "battle_turn/test_battle.cpp", "battle_passives/test_battle.cpp",
                                                "battle_records/test_battle.cpp", "starter/test_battle.cpp",
                                                "starter_battle/battle.cpp", "test_battle/battle.cpp")},
                           "battle_conditions/test_battle.cpp": (EFFECT_HANDLERS_BATTLE
                                                                 + "; a typed damage_hp's program counts the "
                                                                 "before_damage hooks it runs (ProgramInfo "
                                                                 "events_max)"),
                           "test_effects_vm.cpp": ("The VM fixture's move Everything ends with the new primitives "
                                                   "(stage silent, reset_stages, delay, reward, end_battle: opcodes "
                                                   "82-85; events_max with them)"),
                           "battle_moves/battle_ids.h": "The primitives fixture (tests/data/battle_moves), new",
                           "battle_moves/test_battle.cpp": ("The primitives fixture (tests/data/battle_moves), new: "
                                                            "move / side / volatile / weather point handlers, "
                                                            "move_type_point 2, the delayed phase (5), the species "
                                                            "field table read by species_field_of()"),
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in EFFECT_HANDLERS_CHANGES.items()})

## Why every battle output changed in the hit categories: BattleData gains hit_categories after categories (nullptr
## without a hit_category_by_type / category_by_type table), Rules::move_type_point is kNone8 when no handler modifies
## the point, species_field() reads the species key through monster::Role::SpeciesKey (no RoleId{0} literal), and the
## ids header names an unstored custom role UNSTORED_ROLE_<NAME> (no ROLE_ constant: monster::get() reads 0 for it).
HIT_CATEGORIES_BATTLE = "kData's initializer gains hit_categories (nullptr here) after kCategories"
HIT_CATEGORIES_CHANGES = {
   **{name: HIT_CATEGORIES_BATTLE for name in ("battle_conditions/test_battle.cpp", "battle_presenters/test_battle.cpp",
                                               "battle_progress/test_battle.cpp", "battle_resources/test_battle.cpp",
                                               "battle_slots2/test_battle.cpp", "battle_stats3/test_battle.cpp",
                                               "battle_tiers/test_battle.cpp", "battle_vm/test_battle.cpp",
                                               "battle_turn/test_battle.cpp", "battle_passives/test_battle.cpp",
                                               "starter/test_battle.cpp", "starter_battle/battle.cpp",
                                               "test_battle/battle.cpp")},
                          "battle_records/test_battle.cpp": HIT_CATEGORIES_BATTLE + "; species_field() reads "
                          "monster::Role::SpeciesKey",
                          "battle_records/battle_ids.h": "The unstored role marked is UNSTORED_ROLE_MARKED (was "
                          "ROLE_MARKED)",
                          "test_effects_vm.cpp": "The Ref static_assert names the appended ref PlayerSide (57; "
                          "NUM_REFS 58)",
                          "battle_moves/battle_ids.h": ("The fixture's new moves (Sink, Hold, Lash, Retort, Ward, "
                                                        "Finale), volatiles (sink, hold, warded, cheer), category "
                                                        "blast, party Lures and messages held, cheered"),
                          "battle_moves/test_battle.cpp": ("The fixture's new owners, category blast and "
                                                           "kHitCategories (structure hit_category_by_type), party "
                                                           "Lures, messages held / cheered"),
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in HIT_CATEGORIES_CHANGES.items()})

## Why every battle output changed in the inventory (the bag): the battle source includes capsule/inventory/data.h
## and ends with capsule::inventory::kInventory (pockets, the items' bag and use fields, shops; empty tables for a
## battle without pockets), the ids header with POCKET_ / SHOP_ constants (NUM_POCKETS / NUM_SHOPS 0 without).
INVENTORY_BATTLE = ("The source ends with capsule::inventory::kInventory (pockets, ItemInfo per item, shops) and the "
                    "ids header with NUM_POCKETS / NUM_SHOPS")
INVENTORY_CHANGES = {
   **{name: INVENTORY_BATTLE for name in ("battle_conditions/test_battle.cpp", "battle_presenters/test_battle.cpp",
                                          "battle_progress/test_battle.cpp", "battle_resources/test_battle.cpp",
                                          "battle_slots2/test_battle.cpp", "battle_stats3/test_battle.cpp",
                                          "battle_tiers/test_battle.cpp", "battle_vm/test_battle.cpp",
                                          "battle_turn/test_battle.cpp", "battle_passives/test_battle.cpp",
                                          "battle_records/test_battle.cpp", "battle_moves/test_battle.cpp",
                                          "test_battle/battle.cpp")},
   **{name: "NUM_POCKETS / NUM_SHOPS (0)" for name in (
                                                       "battle_conditions/battle_ids.h",
                                                       "battle_presenters/battle_ids.h", "battle_progress/battle_ids.h",
                                                       "battle_resources/battle_ids.h", "battle_slots2/battle_ids.h",
                                                       "battle_stats3/battle_ids.h",
                                                       "battle_tiers/battle_ids.h", "battle_vm/battle_ids.h",
                                                       "battle_turn/battle_ids.h",
                                                       "battle_passives/battle_ids.h", "battle_records/battle_ids.h",
                                                       "battle_moves/battle_ids.h",
                                                       "test_battle/battle_ids.h")},
   **{name: ("The Starter's four pockets, the items' fields and six bag items (Tonic, Mana Tea, Spark Snack, Remedy, "
             "Orb, Gate Pass with their use_field logic and texts), two shops")
      for name in ("starter/test_battle.cpp", "starter/battle_ids.h", "starter_battle/battle.cpp",
                   "starter_battle/battle_ids.h")},
                     "test_effects_vm.cpp": "The VM fixture's item Gem teaches a move on use_field (Op::Teach, "
                     "appended: 86)",
                     "test_script.cpp": "The script fixture's bag script (ItemGive / ItemCount / ItemTake / TimeOfDay "
                     "/ BagOpen)",
                     "test_script_ids.h": "SCRIPT_BAG, VAR_BAG_OK",
                     "inventory/test_battle.cpp": "The inventory fixture (tests/data/inventory), new",
                     "inventory/battle_ids.h": "The inventory fixture (tests/data/inventory), new",
                     "sandbox_party/battle.cpp": "The inventory's sandbox app (the Starter's battle data), new",
                     "sandbox_party/battle_ids.h": "The inventory's sandbox app, new",
                     "sandbox_party/transfer.cpp": "The inventory's sandbox app, new",
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in INVENTORY_CHANGES.items()})

## Why every battle output changed in the AI profiles (recorded together with the inventory's): kVolatiles rows gain
## source (a bound volatile's slot), PartyData ai_profile replaces the AI flags, kAiRules / kAiProfiles / kDelayedHits
## are emitted before the messages and kData ends with them (profile and delayed hit counts, the default profile); the
## ids header names the AI profiles (AI_PROFILE_*, NUM_AI_PROFILES) and the delayed hits (NUM_DELAYED_HITS).
AI_PROFILES_BATTLE = ("kVolatiles source, PartyData ai_profile, kAiRules / kAiProfiles / kDelayedHits and kData's tail "
                      "(ai_profiles, ai_rules, delayed_hits, counts, ai_default)")
AI_PROFILES_CHANGES = {
   **{name: AI_PROFILES_BATTLE for name in ("battle_conditions/test_battle.cpp", "battle_presenters/test_battle.cpp",
                                            "battle_progress/test_battle.cpp", "battle_resources/test_battle.cpp",
                                            "battle_slots2/test_battle.cpp", "battle_stats3/test_battle.cpp",
                                            "battle_tiers/test_battle.cpp", "battle_vm/test_battle.cpp",
                                            "battle_turn/test_battle.cpp", "battle_passives/test_battle.cpp",
                                            "battle_records/test_battle.cpp", "battle_moves/test_battle.cpp",
                                            "starter/test_battle.cpp", "starter_battle/battle.cpp",
                                            "test_battle/battle.cpp",
                                            "inventory/test_battle.cpp", "sandbox_party/battle.cpp")},
   **{name: "AI_PROFILE_* / NUM_AI_PROFILES, NUM_DELAYED_HITS" for name in (
                                                                            "battle_conditions/battle_ids.h",
                                                                            "battle_presenters/battle_ids.h",
                                                                            "battle_progress/battle_ids.h",
                                                                            "battle_resources/battle_ids.h",
                                                                            "battle_slots2/battle_ids.h",
                                                                            "battle_stats3/battle_ids.h",
                                                                            "battle_tiers/battle_ids.h",
                                                                            "battle_vm/battle_ids.h",
                                                                            "battle_turn/battle_ids.h",
                                                                            "battle_passives/battle_ids.h",
                                                                            "battle_records/battle_ids.h",
                                                                            "battle_moves/battle_ids.h",
                                                                            "starter/battle_ids.h",
                                                                            "starter_battle/battle_ids.h",
                                                                            "test_battle/battle_ids.h",
                                                                            "inventory/battle_ids.h",
                                                                            "sandbox_party/battle_ids.h")},
                       "test_effects_vm.cpp": ("The VM fixture's move Everything ends with delay_hit and use_move ... "
                                               "unless (Op DelayHit / CallMove), a global decoy absorbs (Op::Absorb), "
                                               "Ref::Absorbed and Hook::AiScore in the static_asserts"),
                       "battle_gaps/test_battle.cpp": "The gaps fixture (tests/data/battle_gaps), new",
                       "battle_gaps/battle_ids.h": "The gaps fixture (tests/data/battle_gaps), new",
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in AI_PROFILES_CHANGES.items()})


## Input map targets: (compiler target, output name, symbol), as CMakeLists.txt compiles them.
INPUT_MAPS = [
   ("gba", "input_map_gba.cpp", "kInputMap"),
   ("dreamcast", "input_map_dreamcast.cpp", "kInputMap"),
   ("snes_layout", "input_map_snes_layout.cpp", "kInputMap"),
   ("gba", "test_input_map_gba.cpp", "kTestInputMapGba"),
   ("snes_layout", "test_input_map_snes.cpp", "kTestInputMapSnes"),
   ("dreamcast", "test_input_map_dreamcast.cpp", "kTestInputMapDreamcast"),
]
## Record transfer fixtures (cmake/parts/transfer.cmake).
TRANSFER_FIXTURES = ["six", "four", "shuffled", "twin"]
## Test sequences (CAPSULE_TEST_SEQUENCES).
TEST_SEQUENCES = ["title_timeout", "title_song_end", "credits_map", "credits_showcase", "cutscene"]

## Battle data sets of the battle tests (in the snapshot since the effect bank; the classic sample tests/data/battle
## moved to the reference-ruleset repo in the Starter sample): (output folder, manifest under tests/data).
BATTLE_TEST_SETS = [
   ("battle_stats3", "battle_stats3/battle.json"),
   ("battle_slots2", "battle_stats3/slots2.json"),
   ("battle_vm", "effects/battle/battle.json"),
   ("battle_conditions", "effects/conditions/battle.json"),
   ("battle_tiers", "battle_tiers/battle.json"),
   ("battle_turn", "battle_turn/battle.json"),
   ("battle_progress", "battle_progress/battle.json"),
   ("battle_passives", "battle_progress/passives.json"),
   ("battle_records", "battle_progress/records.json"),
   ("battle_resources", "battle_resources/battle.json"),
   ("battle_presenters", "battle_presenters/battle.json"),
   ("battle_moves", "battle_moves/battle.json"),
   ("inventory", "inventory/battle.json"),
   ("battle_gaps", "battle_gaps/battle.json"),
   ("growth", "growth/battle.json"),
   ("recruit", "recruit/battle.json"),
   # F1: the field rules fixture (cmake/parts/field_tests.cmake)
   ("field", "field/battle.json"),
]
## The step effects fixture (cmake/parts/field_tests.cmake step_effects_tests), apart from BATTLE_TEST_SETS so
## the earlier packages' recorded changes do not name it.
STEP_TEST_SETS = [("steps", "steps/battle.json")]
## The combination's leftover: the combination fixture (cmake/parts/combine_tests.cmake combine_tests / nursery_tests),
## apart from BATTLE_TEST_SETS so the earlier packages' recorded changes do not name it.
COMBINE_TEST_SETS = [("combine", "combine/battle.json")]
## The Starter sample (cmake/parts/battle_tests.cmake starter_golden_tests): output folder, manifest under the
## repository root (its game, apps/starter_battle, is in game_runs()).
STARTER_SET = ("starter", "samples/starter/battle.json")

## Why every battle output changed in the growth (growth): the battle source includes capsule/growth/data.h
## and ends with rules::growth_when(), rules::train_gain() and capsule::growth::kGrowth (empty tables for data without
## growth); the ids gain TRIGGER_ / CONTEXT_ / FORM_ constants.
GROWTH_BATTLE = ("The source ends with rules::growth_when() / train_gain() and capsule::growth::kGrowth "
                 "(transformation rules, forms, context values, training; empty without them)")
## Why the ids headers changed in the growth.
GROWTH_IDS = "TRIGGER_ (5), NUM_CONTEXTS / CONTEXT_, FORM_BASE / NUM_FORM_NAMES"
## Why the Starter's outputs changed in the growth.
GROWTH_STARTER = ("The Starter's growth: ten stage-two species (keys after the older ones), Bloom Charm, Glow Charm "
                  "and Drill Notes, the stat_training (6 bits) and form (1 bit) roles, training in the stat formulas, "
                  "yield species fields, Glimmet's Radiant form and its sprites")
GROWTH_CHANGES = {
   **{f"{folder}/test_battle.cpp": GROWTH_BATTLE for folder, _m in BATTLE_TEST_SETS if folder != "growth"},
   **{f"{folder}/battle_ids.h": GROWTH_IDS for folder, _m in BATTLE_TEST_SETS if folder != "growth"},
   **{name: GROWTH_BATTLE for name in ("test_battle/battle.cpp", "sandbox_party/battle.cpp")},
   **{name: GROWTH_IDS for name in ("test_battle/battle_ids.h", "sandbox_party/battle_ids.h")},
   **{name: GROWTH_STARTER for name in ("starter/test_battle.cpp", "starter/battle_ids.h", "starter_battle/battle.cpp",
                                        "starter_battle/battle_ids.h", "starter_battle/assets.cpp",
                                        "starter_battle/transfer.cpp", "sandbox_party/transfer.cpp",
                                        "test_battle/transfer.cpp", "transfer/test_transfer.cpp",
                                        "transfer/transfer_cases.h", "transfer/transfer_twin.cpp")},
                  "test_effects_vm.cpp": "The VM fixture's item Stone (Op::Form, Op::Transform, Op::Train, appended)",
                  "test_script.cpp": "The script fixture's grow script (the Transform op)",
                  "test_script_ids.h": "SCRIPT_GROW",
                  "growth/test_battle.cpp": "The growth fixture (tests/data/growth), new",
                  "growth/battle_ids.h": "The growth fixture (tests/data/growth), new",
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in GROWTH_CHANGES.items()})

## Why every battle output changed in the recruitment (recruitment and storage): hook masks are HookMask words
## ({{0x..u}}), parties carry their items, the source includes capsule/storage/storage.h and ends with
## capsule::battle::kRecruit and capsule::storage::kStorage (no checks, no boxes without the sections).
RECRUIT_BATTLE = ("Hook masks as HookMask words, PartyData items, capsule::battle::kRecruit and "
                  "capsule::storage::kStorage (empty without recruit / storage)")
## Why the ids headers changed in the recruitment.
RECRUIT_IDS = "NUM_BOXES / BOX_SIZE"
## Why the Starter's outputs changed in the recruitment.
RECRUIT_STARTER = ("The Starter's recruitment: the Orb's battle use, the recruit_rate species field, the recruit "
                   "section (3 checks) and recruit_odds, 8 boxes of 30, Scholar Wen's Tonic and the Strategist's item "
                   "rule")
## The recruitment changes, appended to each output's history.
RECRUIT_CHANGES = {
   **{f"{folder}/test_battle.cpp": RECRUIT_BATTLE for folder, _m in BATTLE_TEST_SETS if folder not in ("recruit",
                                                                                                       "growth")},
   **{f"{folder}/battle_ids.h": RECRUIT_IDS for folder, _m in BATTLE_TEST_SETS if folder not in ("recruit", "growth")},
   **{name: RECRUIT_BATTLE for name in ("test_battle/battle.cpp", "sandbox_party/battle.cpp")},
   **{name: RECRUIT_IDS for name in ("test_battle/battle_ids.h", "sandbox_party/battle_ids.h")},
   **{name: RECRUIT_STARTER for name in ("starter/test_battle.cpp", "starter/battle_ids.h", "starter_battle/battle.cpp",
                                         "starter_battle/battle_ids.h")},
                   "test_effects_vm.cpp": "Hook masks as HookMask words; the VM fixture's item Net (Op::Recruit, "
                   "appended)",
                   "test_script.cpp": "The script fixture's boxes script (BoxCount / BoxDeposit / BoxWithdraw / "
                   "BoxesOpen)",
                   "test_script_ids.h": "SCRIPT_BOXES",
                   "recruit/test_battle.cpp": "The recruit fixture (tests/data/recruit), new",
                   "recruit/battle_ids.h": "The recruit fixture (tests/data/recruit), new",
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in RECRUIT_CHANGES.items()})

## Why every battle output changed in the save shapes (review4 fixes): the items' stable save keys (kItemKeys, the last
## field of kInventory), kData's weather_suppress_tags and teachable tables (0 / nullptr unless the manifest names
## them), TrainingRules::gain_point (0xFF: none), the hook list's weather_changed (hook masks of owners with it).
SAVE_SHAPES_BATTLE = ("kItemKeys + InventoryData {..., {0, 0}, kItemKeys}; kData weather_suppress_tags, teach_first / "
                      "teach_moves; TrainingRules gain_point")
## Why the ids headers changed in the save shapes.
SAVE_SHAPES_IDS = "STORAGE_BYTES / STORAGE_USED_BYTES (the storage sized from the manifest)"
## Why every script bank changed in the save shapes.
SAVE_SHAPES_SCRIPTS = "Bank::save_remap_count, save_hashes (a shape hash per save sub-bank), save_remaps, save_moves"
## The save shapes changes, appended to each output's history.
SAVE_SHAPES_CHANGES = {
   **{f"{folder}/test_battle.cpp": SAVE_SHAPES_BATTLE for folder, _m in BATTLE_TEST_SETS},
   **{f"{folder}/battle_ids.h": SAVE_SHAPES_IDS for folder, _m in BATTLE_TEST_SETS},
   **{name: SAVE_SHAPES_BATTLE for name in ("test_battle/battle.cpp", "sandbox_party/battle.cpp",
                                            "starter_battle/battle.cpp",
                                            "starter/test_battle.cpp")},
   **{name: SAVE_SHAPES_IDS for name in ("test_battle/battle_ids.h", "sandbox_party/battle_ids.h",
                                         "starter_battle/battle_ids.h",
                                         "starter/battle_ids.h")},
   **{name: SAVE_SHAPES_SCRIPTS for name in ("sandbox_map/scripts.cpp", "test_map_scripts.cpp", "test_script.cpp",
                                             "test_script/scripts.cpp")},
                       "test_effects_vm.cpp": "The hook list's weather_changed (hook mask words unchanged)",
                       "battle_turn/test_battle.cpp": SAVE_SHAPES_BATTLE
                       + "; the fixture's Vane / Calm passives, Vanebird / Calmer, the calm tag",
                       "inventory/test_battle.cpp": SAVE_SHAPES_BATTLE + "; the fixture's Mixed Brew and Blob's "
                       "teachable list",
                       "growth/test_battle.cpp": SAVE_SHAPES_BATTLE + "; the fixture's Heavy Band and training point",
                       "recruit/test_battle.cpp": SAVE_SHAPES_BATTLE + "; the fixture's refusal_consumes [blocked]",
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in SAVE_SHAPES_CHANGES.items()})

## Why every battle and transfer output changed in the battle layout (save fixes): capsule::monster::Layout ends with
## record_hash, the layout hash without the party size (what the save image's party and boxes sections check).
RECORD_HASH_LAYOUT = "kLayout ends with record_hash (`..., {0}, 0x...u};`): the layout hash without the party size"
## Why the script banks with ranged variables changed in the battle layout.
RECORD_HASH_SCRIPTS = ("save_hashes of the variable sub-banks holding a variable with a declared range (the range is "
                       "part of the save shape now; variables of their type's default range keep their hash)")
## The battle layout changes, appended to each output's history.
RECORD_HASH_CHANGES = {
   **{f"{folder}/test_battle.cpp": RECORD_HASH_LAYOUT for folder, _m in BATTLE_TEST_SETS},
   **{name: RECORD_HASH_LAYOUT for name in ("test_battle/battle.cpp", "sandbox_party/battle.cpp",
                                            "starter_battle/battle.cpp",
                                            "starter/test_battle.cpp", "transfer/transfer_six.cpp",
                                            "transfer/transfer_four.cpp", "transfer/transfer_shuffled.cpp",
                                            "transfer/transfer_twin.cpp")},
   **{name: RECORD_HASH_SCRIPTS for name in ("sandbox_map/scripts.cpp", "test_map_scripts.cpp", "test_script.cpp",
                                             "test_script/scripts.cpp")},
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in RECORD_HASH_CHANGES.items()})

## Why every battle output changed in F1 (encounter leads and timed field effects): the source includes
## capsule/battle/data/field.h and capsule/script/data.h and ends with capsule::battle::kFieldRules (the lead rule,
## the field effect count), the two encounter pipelines are emitted (rules::encounter_rate / encounter_level: the
## defaults without them), the hook list ends with encounter / encounter_made, and the ids header ends with the
## FIELD_EFFECT_ ids and NUM_FIELD_EFFECTS.
FIELD_RULES_BATTLE = ("kFieldRules at the end (kNoLeadRule and 0 without lead / field_effects), "
                      "rules::encounter_rate() and encounter_level() emitted, the encounter hooks after "
                      "weather_changed")
## Why the ids headers changed in F1.
FIELD_RULES_IDS = "NUM_FIELD_EFFECTS (and FIELD_EFFECT_<NAME> per structure field effect) at the end"
## Why every script bank changed in F1.
FIELD_RULES_SCRIPTS = ("capsule::script::Bank ends with field_effects and field_effect_count (`nullptr, 0,` without "
                       "field effects); a battle with field effects gives each one an implicit permanent u16 variable "
                       "and, with an expiry message, an implicit script")
## Why the Starter's, the stats3 fixture's and the map sandbox's outputs changed further in F1.
FIELD_RULES_CONTENT = ("the Starter's Ward charm, its `ward` field effect, the global ward rule and Quick Feet's "
                       "encounter handler; tests/data/battle_stats3's ward / glow / lure field effects "
                       "(tests/data/scripts' `fx` script and implicit variables); apps/sandbox_map names the Starter's "
                       "battle (its own battle data and the Ward's implicit variable and expiry script)")
## The F1 changes, appended to each output's history.
FIELD_RULES_CHANGES = {
   **{f"{folder}/test_battle.cpp": FIELD_RULES_BATTLE for folder, _m in BATTLE_TEST_SETS if folder != "field"},
   **{f"{folder}/battle_ids.h": FIELD_RULES_IDS for folder, _m in BATTLE_TEST_SETS if folder != "field"},
   **{name: FIELD_RULES_BATTLE for name in ("test_battle/battle.cpp", "sandbox_party/battle.cpp",
                                            "starter_battle/battle.cpp",
                                            "starter/test_battle.cpp")},
   **{name: FIELD_RULES_IDS for name in ("test_battle/battle_ids.h", "sandbox_party/battle_ids.h",
                                         "starter_battle/battle_ids.h", "starter/battle_ids.h")},
   **{name: FIELD_RULES_SCRIPTS for name in ("test_map_scripts.cpp", "test_script.cpp", "test_script/scripts.cpp",
                                             "sandbox_map/scripts.cpp")},
                       "test_effects_vm.cpp": "tests/data/effects/vm_ops.ceffect's item Charm (the Encounter and "
                       "FieldEffect ops)",
                       "test_script_ids.h": FIELD_RULES_CONTENT, "sandbox_map/script_ids.h": FIELD_RULES_CONTENT,
}
for name in ("starter/test_battle.cpp", "starter_battle/battle.cpp", "sandbox_party/battle.cpp",
             "test_battle/battle.cpp", "battle_stats3/test_battle.cpp", "battle_slots2/test_battle.cpp",
             "test_script.cpp", "sandbox_map/scripts.cpp"):
   FIELD_RULES_CHANGES[name] += "; " + FIELD_RULES_CONTENT
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in FIELD_RULES_CHANGES.items()})

## kFieldRules gains the field action count and their first growth rule (all battle data).
FIELD_ACTIONS_BATTLE = "kFieldRules {lead, effects, field actions, first action rule, 0} (8 B)"
## The Starter's field actions (wade, clear, light) and the fixture's (paddle, shove, lamp, rope).
FIELD_ACTIONS_ACTIONS = ("The structure's field_actions: their requirements as growth rules (knows.MOVE, passive, "
                         "flag. / var. through rules::field_state()) and FIELD_ACTION_ ids")
## The scripts' side (field action records, references, flight points, the member and escape variables).
FIELD_ACTIONS_SCRIPTS = ("The bank's field actions / references / flight points / member and escape variables, the "
                         "implicit member variable, asking wrappers, flight flags and escape variables, the darkness / "
                         "flight / escape statements")
## The worlds (an object's kind byte in ObjectTemplate, a map's darkness / tags / escape point, dismount).
FIELD_ACTIONS_WORLD = ("ObjectTemplate's kind byte (the reserved bytes 2), the test world's paddle class (dismount), "
                       "the pacer's kind, small's darkness 2 and tag dark, field's escape point")
FIELD_ACTIONS_CHANGES = {
   **{f"{folder}/test_battle.cpp": FIELD_ACTIONS_BATTLE for folder, _m in BATTLE_TEST_SETS},
   **{name: FIELD_ACTIONS_BATTLE for name in ("test_battle/battle.cpp", "sandbox_party/battle.cpp",
                                              "starter_battle/battle.cpp",
                                              "starter/test_battle.cpp")},
   **{name: FIELD_ACTIONS_ACTIONS for name in ("field/battle_ids.h", "starter/battle_ids.h",
                                               "starter_battle/battle_ids.h",
                                               "sandbox_party/battle_ids.h", "test_battle/battle_ids.h",
                                               "sandbox_map/battle_ids.h")},
   **{name: FIELD_ACTIONS_SCRIPTS for name in ("field_script.cpp", "field_script_ids.h", "test_map_scripts.cpp",
                                               "test_map_script_ids.h", "test_script.cpp", "test_script_ids.h",
                                               "sandbox_map/scripts.cpp", "sandbox_map/script_ids.h")},
                         "test_world.cpp": FIELD_ACTIONS_WORLD,
                         "sandbox_map/world.cpp": ("The sandbox world: swim only on water with dismount, the bush "
                                                   "object (kind bush), the cave's darkness 3 and tag dark and its "
                                                   "sign, escape points showcase and meadow"),
                         "sandbox_map/world_ids.h": "The showcase's object OBJ_SHOWCASE_BUSH",
                         "sandbox_map/assets.cpp": "sprite:BUSH (shapes)",
                         "sandbox_map/battle.cpp": FIELD_ACTIONS_BATTLE,
}
for name in ("field/test_battle.cpp", "starter/test_battle.cpp", "starter_battle/battle.cpp",
             "sandbox_party/battle.cpp", "test_battle/battle.cpp", "sandbox_map/battle.cpp"):
   FIELD_ACTIONS_CHANGES[name] += "; " + FIELD_ACTIONS_ACTIONS
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in FIELD_ACTIONS_CHANGES.items()})

## kFieldRules' former reserved u16 holds the step effect count and a reserved byte (all battle
## data: `, 0, 0}` at its end).
STEP_EFFECTS_BATTLE = "kFieldRules {lead, effects, field actions, first action rule, step effects, 0}"
## The Starter's step effects (blight, bond), its bond role, Blight's steps handler and the global bond rule.
STEP_EFFECTS_STARTER = ("The Starter's custom role bond (8 bits, after form), its step_effects blight (every 4, "
                        "survive, message blight_survived) and bond (every 64), Blight's `on steps` and the global "
                        "bond_rule; STEP_EFFECT_ ids, ROLE_BOND")
## The record layouts that follow the Starter's (the bond role).
STEP_EFFECTS_TRANSFER = "The Starter's layout (and its twin's) gains the custom role bond (8 bits)"
## The map sandbox's scripts (the Starter's step effects' implicit counters).
STEP_EFFECTS_SCRIPTS = ("The Starter's step effects' implicit counters step_effect_blight / step_effect_bond and the "
                        "bank's kStepEffects records")
STEP_EFFECTS_CHANGES = {
   **{f"{folder}/test_battle.cpp": STEP_EFFECTS_BATTLE for folder, _m in BATTLE_TEST_SETS},
   **{name: STEP_EFFECTS_BATTLE + "; " + STEP_EFFECTS_STARTER for name in ("starter/test_battle.cpp",
                                                                           "starter_battle/battle.cpp",
                                                                           "sandbox_party/battle.cpp",
                                                                           "test_battle/battle.cpp",
                                                                           "sandbox_map/battle.cpp")},
   **{name: STEP_EFFECTS_STARTER for name in ("starter/battle_ids.h", "starter_battle/battle_ids.h",
                                              "sandbox_party/battle_ids.h", "test_battle/battle_ids.h",
                                              "sandbox_map/battle_ids.h")},
   **{name: STEP_EFFECTS_TRANSFER for name in ("starter_battle/transfer.cpp", "sandbox_party/transfer.cpp",
                                               "test_battle/transfer.cpp", "sandbox_map/transfer.cpp",
                                               "transfer/test_transfer.cpp", "transfer/transfer_cases.h",
                                               "transfer/transfer_twin.cpp")},
   **{name: STEP_EFFECTS_SCRIPTS for name in ("sandbox_map/scripts.cpp", "sandbox_map/script_ids.h")},
                        "test_effects_vm.cpp": "The hook list ends with steps (Hook::Steps)",
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in STEP_EFFECTS_CHANGES.items()})

## The field effect review: kFieldRules gains the hooks' member's rule and a reserved u16 (all battle data: `, 0xFFFF,
## 0}` at its end; the field fixture's hook_when rule moves the field actions' rules one on).
HOOK_LEAD_BATTLE = "kFieldRules {..., 0, hook_lead_rule, 0} (lead.hook_when)"
## The script banks: the escape point's map asset key in two u16 halves (EscapeVars 12 bytes), the map-lifetime
## darkness variable map_darkness of a world with a dark map, the field actions initializer's darkness_var.
HOOK_LEAD_SCRIPTS = ("escape_map_hi / escape_map_lo (the map's asset key) instead of escape_map (index + 1), the "
                     "map-lifetime map_darkness variable, Bank::darkness_var")
## The worlds' darkness levels (World::darkness_levels: kDarknessLevels).
HOOK_LEAD_WORLD = "kDarknessLevels and World's darkness_levels / darkness_level_count"
HOOK_LEAD_CHANGES = {
   **{f"{folder}/test_battle.cpp": HOOK_LEAD_BATTLE for folder, _m in BATTLE_TEST_SETS},
   **{name: HOOK_LEAD_BATTLE for name in ("starter/test_battle.cpp", "starter_battle/battle.cpp",
                                          "sandbox_party/battle.cpp",
                                          "test_battle/battle.cpp", "sandbox_map/battle.cpp")},
   **{name: HOOK_LEAD_SCRIPTS for name in ("field_script.cpp", "field_script_ids.h", "test_map_scripts.cpp",
                                           "test_map_script_ids.h", "steps_script.cpp", "sandbox_map/scripts.cpp",
                                           "sandbox_map/script_ids.h")},
                     "test_script.cpp": HOOK_LEAD_SCRIPTS
                     + "; the checked walk test script checked_walk (op ObjectMoveChecked), var walk_ok",
                     "test_script_ids.h": "SCRIPT_CHECKED_WALK, VAR_WALK_OK",
                     "steps/test_battle.cpp": HOOK_LEAD_BATTLE,
                     "test_world.cpp": HOOK_LEAD_WORLD + " (the fixture's own three levels)",
                     "sandbox_map/world.cpp": HOOK_LEAD_WORLD + " (the Starter's / sandbox's: the former engine table)",
}
HOOK_LEAD_CHANGES["field/test_battle.cpp"] += "; the field fixture's lead.hook_when (growth rule 1)"
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in HOOK_LEAD_CHANGES.items()})

## Every battle data's combination block (rules::combine_value(), the combine tables, the
## combine_odds / combine_fee pipelines' defaults, the capsule/combine/data.h include).
COMBINATION_BATTLE = "The combination block (combine_value, kCombine tables, combine_odds / combine_fee defaults)"
## The Starter's merge_altar method (its species' combine_moves, its messages, the global merge_rule).
COMBINATION_STARTER = ("The Starter's combine method merge_altar (a type table, consumed), the base species' "
                       "combine_moves, messages merge_unfit / merge_refused / merge_done / merge_no_room and the "
                       "global merge_rule (on combine); COMBINE_METHOD_MERGE_ALTAR")
## The scripts using `combine` (the map sandbox's merge_altar, the script fixture's mix).
COMBINATION_SCRIPTS = "Op Combine (the script merge_altar: combine open merge_altar -> var merged)"
COMBINATION_CHANGES = {
   **{f"{folder}/test_battle.cpp": COMBINATION_BATTLE for folder, _m in BATTLE_TEST_SETS},
   **{name: COMBINATION_BATTLE + "; " + COMBINATION_STARTER for name in ("starter/test_battle.cpp",
                                                                         "starter_battle/battle.cpp",
                                                                         "sandbox_party/battle.cpp",
                                                                         "test_battle/battle.cpp",
                                                                         "sandbox_map/battle.cpp")},
   **{name: COMBINATION_STARTER for name in ("starter/battle_ids.h", "starter_battle/battle_ids.h",
                                             "sandbox_party/battle_ids.h", "test_battle/battle_ids.h",
                                             "sandbox_map/battle_ids.h")},
                       "battle_stats3/battle_ids.h": "The fixture's combine method pair (COMBINE_METHOD_PAIR)",
                       "sandbox_map/scripts.cpp": COMBINATION_SCRIPTS + "; the variable merged",
                       "sandbox_map/script_ids.h": "SCRIPT_MERGE_ALTAR, VAR_MERGED",
                       "test_script.cpp": "The combine test script mix (combine open / run / preview / status / fee)",
                       "test_script_ids.h": "SCRIPT_MIX",
                       "test_effects_vm.cpp": "Op Inherit, the hook list ends with combine (Hook::Combine), the owner "
                       "mix",
}
COMBINATION_CHANGES["battle_stats3/test_battle.cpp"] += "; the fixture's combine method pair"
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in COMBINATION_CHANGES.items()})

## Combination sections 2 and 3: every battle data's combination block (the EggData / NurseryData initializers of
## kCombine, MethodData's cycle_steps) and the nursery pipelines' defaults (nursery_gain, nursery_fee).
NURSERY_BATTLE = "kCombine's egg and nursery tables, MethodData::cycle_steps, nursery_gain / nursery_fee defaults"
## The Starter's Grove (structure eggs / nursery, method grove, step effects grove_check / egg_cycle, the egg and
## hatch_counter roles, species fields kin / hatch_cycles, the hidden passive slot and Nurture).
NURSERY_STARTER = ("The Starter's Grove nursery (method grove: kin, an egg of 5 x 256 steps), its eggs (roles egg, "
                   "hatch_counter; held_item 8 bits: 64-byte records kept), step effects grove_check / egg_cycle, "
                   "species fields kin / hatch_cycles, the hidden passive slot and the talent Nurture (on hatch), 8 "
                   "boxes of 28 (were 30); NURSERY_SLOTS")
## The record transfer tables of the Starter's layout (the egg roles) and its twin fixture.
NURSERY_TRANSFER = ("The Starter's layout with the egg and hatch_counter roles, held_item 8 bits (transfer hash, "
                    "roles); the cases' Starter record holds item 200")
NURSERY_CHANGES = {
   **{f"{folder}/test_battle.cpp": NURSERY_BATTLE for folder, _m in BATTLE_TEST_SETS + STEP_TEST_SETS},
   **{name: NURSERY_BATTLE + "; " + NURSERY_STARTER for name in ("starter/test_battle.cpp", "starter_battle/battle.cpp",
                                                                 "sandbox_party/battle.cpp", "test_battle/battle.cpp",
                                                                 "sandbox_map/battle.cpp")},
   **{name: NURSERY_STARTER for name in ("starter/battle_ids.h", "starter_battle/battle_ids.h",
                                         "sandbox_party/battle_ids.h", "test_battle/battle_ids.h",
                                         "sandbox_map/battle_ids.h")},
   **{name: NURSERY_TRANSFER for name in ("starter_battle/transfer.cpp", "sandbox_party/transfer.cpp",
                                          "test_battle/transfer.cpp", "sandbox_map/transfer.cpp",
                                          "transfer/test_transfer.cpp", "transfer/transfer_twin.cpp",
                                          "transfer/transfer_cases.h")},
                   "sandbox_map/scripts.cpp": ("Op Nursery (the scripts grove and hatch_egg), the step effects "
                                               "grove_check / egg_cycle, egg_member and the Bank's hatch members"),
                   "sandbox_map/script_ids.h": "SCRIPT_GROVE, SCRIPT_HATCH_EGG, VAR_GROVE_*, VAR_NAMED, "
                   "VAR_EGG_MEMBER, ...",
                   "test_effects_vm.cpp": "The hook list ends with hatch (Hook::Hatch)",
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in NURSERY_CHANGES.items()})

## The combination review's fixes: every battle data's EggData gains step_effects (the step effects' allow_eggs mask)
## and its flags the lead / transform / transfer bits; MoveSourceData's last byte is `from` (0xFF: every input).
EGG_FLAGS_BATTLE = ("EggData::step_effects (a 0 mask), MoveSourceData::from (0xFF every input; the Starter's sources "
                    "and the padding ones)")
EGG_FLAGS_CHANGES = {
   **{name: EGG_FLAGS_BATTLE for name in ("battle_conditions/test_battle.cpp", "battle_gaps/test_battle.cpp",
                                          "battle_moves/test_battle.cpp", "battle_passives/test_battle.cpp",
                                          "battle_presenters/test_battle.cpp", "battle_progress/test_battle.cpp",
                                          "battle_records/test_battle.cpp", "battle_resources/test_battle.cpp",
                                          "battle_slots2/test_battle.cpp", "battle_stats3/test_battle.cpp",
                                          "battle_tiers/test_battle.cpp", "battle_turn/test_battle.cpp",
                                          "battle_vm/test_battle.cpp", "field/test_battle.cpp",
                                          "growth/test_battle.cpp",
                                          "inventory/test_battle.cpp", "recruit/test_battle.cpp",
                                          "steps/test_battle.cpp",
                                          "starter/test_battle.cpp", "starter_battle/battle.cpp",
                                          "sandbox_party/battle.cpp",
                                          "test_battle/battle.cpp", "sandbox_map/battle.cpp")},
                     "combine/test_battle.cpp": (EGG_FLAGS_BATTLE
                                                 + "; the fixture's item Glow Moss (on steps), nest's input.0.item "
                                                 "read, synthesis' fee and the method brood (an immediate egg method: "
                                                 "sources from b, push)"),
                     "combine/battle_ids.h": "COMBINE_METHOD_BROOD, NUM_COMBINE_METHODS 5, ITEM_GLOW_MOSS",
}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in EGG_FLAGS_CHANGES.items()})

## The map compiler emits an array equal to one emitted before once; later maps name the first.
IMPORTED_MAPS_WORLD = ("Equal per-map arrays (tiles, palettes, metatiles, cells, layer cells, grid, border, animation "
                       "frames) are emitted once and later maps name the first one; every map's contents are unchanged")
## The test world's fixed-placement tileset, its maps fixed_a / fixed_b and the walled behavior fence_east.
IMPORTED_MAPS_FIXTURE = ("The fixed tileset (raw tilemap entries, its two banks, an animation with phase 1), maps "
                         "fixed_a / fixed_b (\"metatiles\": \"all\"), behavior fence_east (walls right: "
                         "Behavior::walls 8)")
IMPORTED_MAPS_CHANGES = {"test_world.cpp": IMPORTED_MAPS_WORLD + "; " + IMPORTED_MAPS_FIXTURE,
                         "sandbox_map/world.cpp": IMPORTED_MAPS_WORLD,
                         "test_world_ids.h": "MAP_FIXED_A, MAP_FIXED_B"}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in IMPORTED_MAPS_CHANGES.items()})

## An object template's and page's two reserved bytes are movement_arg and movement_flags (the
## movements' arguments, "Movement arguments" in capsule/map/map.h; 0x00 for the old movements).
IMPORTED_SPRITES_OBJECTS = ("ObjectTemplate / ObjectPage write movement_arg, movement_flags (0x00, 0x00) for the "
                            "reserved {0, 0}")
## The test world's map crowd (an object view, the reference movement types, a counter, door / arrow / none /
## dynamic warps, stacked objects), the vehicle class trek (turn_frames, bumps, keeps_moving) and the behavior
## counter_top; the assets fixture's streamed sprite STEPPER (frames from an indexed sheet, a step table, flips).
IMPORTED_SPRITES_FIXTURE = ("Map crowd (object_view, 31 objects with the new movements, warps of each trigger, a "
                            "dynamic one), vehicle class trek, behavior counter_top (BehaviorKind::Counter)")
IMPORTED_SPRITES_CHANGES = {"test_world.cpp": IMPORTED_SPRITES_OBJECTS + "; " + IMPORTED_SPRITES_FIXTURE,
                            "sandbox_map/world.cpp": IMPORTED_SPRITES_OBJECTS,
                            "test_world_ids.h": "MAP_CROWD and its objects' local IDs",
                            "test_assets.cpp": ("sprite:STEPPER (kSpriteStreamed, its step table and an animation's "
                                                "flips)")}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in IMPORTED_SPRITES_CHANGES.items()})

## Why the script port outputs changed (the statements imported event scripts need).
SCRIPT_PORTS_CHANGES = {"test_script.cpp": ("Scripts ports, port_other, port_loop, port_resume, port_warp_into (locks, "
                                            "MessageEx, WaitObject, removal, visibility, templates, doors, weather, "
                                            "fades, metatiles, the respawn point, SetWarp, sound, marts, WaitState, "
                                            "CallLoose, Jump) and town's on_resume / on_warp_into triggers"),
                        "test_script_ids.h": "SCRIPT_PORTS, SCRIPT_PORT_OTHER, SCRIPT_PORT_LOOP, SCRIPT_PORT_RESUME, "
                                             "SCRIPT_PORT_WARP_INTO"}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in SCRIPT_PORTS_CHANGES.items()})

## Why the doors outputs changed (doors, override sets, direction grids, the draw order, animation targets): the
## sandbox's hut corner (an engine sandbox scene per feature).
DOORS_SANDBOX = ("Doors sandbox: the flowers animation's four frames on four targets (offsets 0-3), the hut (door:HUT "
                 "bound by the metatile hut_door, map hut, its warps), the garden override set (flag garden_planted) "
                 "and sign, the porter / keeper / host objects")
DOORS_CHANGES = {"sandbox_map/world.cpp": DOORS_SANDBOX + " (kAnimTargets, kDoorBindings, kOverrideSets)",
                 "sandbox_map/world_ids.h": "Doors sandbox: MAP_HUT, the new objects, OVERRIDE_GARDEN",
                 "sandbox_map/assets.cpp": ("Doors sandbox: sprite:NPC as a three-column direction grid (right mirrors "
                                            "left; its right frames gone), sprite:GUARD as a four-column one, door:HUT "
                                            "(kDoors)"),
                 "sandbox_map/scripts.cpp": DOORS_SANDBOX + " (scripts garden_sign, porter_talk, keeper_talk; Op "
                 "Override)",
                 "sandbox_map/script_ids.h": "Doors sandbox: SCRIPT_GARDEN_SIGN, SCRIPT_PORTER_TALK, "
                 "SCRIPT_KEEPER_TALK, "
                 "FLAG_GARDEN_PLANTED"}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in DOORS_CHANGES.items()})

## Why the field options outputs changed (review's fixes): every script bank has the session lifetime (a fifth
## sub-bank size in its flag and variable sizes; a review finding) and ends with Bank::vm_flags (the manifest's "vm"
## options; 0 for these: a review findings).
FIELD_OPTIONS_BANK = "Bank::vm_flags (0) after object_base, the session lifetime's sizes (a review findings)"
FIELD_OPTIONS_CHANGES = {name: FIELD_OPTIONS_BANK for name in ("combine_script.cpp", "field_script.cpp",
                                                               "sandbox_map/scripts.cpp",
                                                               "steps_script.cpp", "test_map_scripts.cpp",
                                                               "test_script.cpp",
                                                               "test_script/scripts.cpp")}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in FIELD_OPTIONS_CHANGES.items()})

## Why the text windows outputs changed: a proportional font's two reserved bytes are glyph_height (the rows a
## text window draws, 0 = the whole cell) and one reserved byte: `..., letter_spacing, 0, 0}` instead of `{0, 0}}`.
TEXT_WINDOWS_FONT = "PropFont::glyph_height (0) and a reserved byte replace reserved[2]"
TEXT_WINDOWS_CHANGES = {name: TEXT_WINDOWS_FONT for name in ("sandbox_menu/assets.cpp", "sandbox_screen/assets.cpp")}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in TEXT_WINDOWS_CHANGES.items()})

## Why the Starter's outputs changed in (the release): the Starter's Cosmic contact move (id 22) has an invented
## name, Orbit Knock (same length and numbers; the name it had was borrowed): its name text, its comments and its id
## MOVE_ORBIT_KNOCK. Nothing else changed.
RELEASE_RENAME = "The Starter's move 22 is named Orbit Knock (its name text, comments and MOVE_ORBIT_KNOCK)"
RELEASE_CHANGES = {name: RELEASE_RENAME for name in ("sandbox_map/battle.cpp", "sandbox_map/battle_ids.h",
                                                     "sandbox_party/battle.cpp", "sandbox_party/battle_ids.h",
                                                     "starter/battle_ids.h", "starter/test_battle.cpp",
                                                     "starter_battle/battle.cpp", "starter_battle/battle_ids.h",
                                                     "test_battle/battle.cpp", "test_battle/battle_ids.h")}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in RELEASE_CHANGES.items()})

## Why the script outputs changed for trainer encounters (D-36 package 2 C1: Encounter 87 / Battle 88).
ENCOUNTER_CHANGES = {"test_script.cpp": ("The encounter test scripts fight, fight_start, fight_after, lone_battle "
                                         "and the flags f_beaten_a / f_beaten_b"),
                     "test_script_ids.h": ("SCRIPT_FIGHT, SCRIPT_FIGHT_START, SCRIPT_FIGHT_AFTER, SCRIPT_LONE_BATTLE, "
                                           "FLAG_F_BEATEN_A, FLAG_F_BEATEN_B"),
                     "sandbox_map/scripts.cpp": ("rival_battle through the encounter record and a party battle "
                                                 "(the engine's default battle port: won at once)")}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in ENCOUNTER_CHANGES.items()})

## Why the script outputs changed again: Encounter Text gained a flags byte (MessageEx's kMessage... bits).
ENCOUNTER_TEXT_FLAGS = "Encounter Text's flags byte after its text byte (0: waits until typed)"
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in {"test_script.cpp": ENCOUNTER_TEXT_FLAGS,
                                           "sandbox_map/scripts.cpp": ENCOUNTER_TEXT_FLAGS}.items()})

## Why the script outputs changed for spotter groups (C1b: encounter load / approach / next, side C).
SPOTTER_GROUP_CHANGES = {"test_script.cpp": ("The spotter group test scripts spot_group, spot_approach, spot_after, "
                                             "spotter_one, spotter_none, spotter_two and spot_single"),
                         "test_script_ids.h": ("SCRIPT_SPOT_GROUP, SCRIPT_SPOT_APPROACH, SCRIPT_SPOT_AFTER, "
                                               "SCRIPT_SPOTTER_ONE, SCRIPT_SPOTTER_NONE, SCRIPT_SPOTTER_TWO, "
                                               "SCRIPT_SPOT_SINGLE (SCRIPT_LONE_BATTLE and later ids move up)")}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in SPOTTER_GROUP_CHANGES.items()})

## Why the script outputs changed for props and object draw order (C2: the Prop and ObjectOrder ops).
PROP_CHANGES = {"test_script.cpp": "The props test script (prop place / face / remove, order)",
                "test_script_assets.cpp": "The props test sprite sprite:PROP_TEST",
                "test_script_ids.h": "SCRIPT_PROPS (SCRIPT_LONE_BATTLE and later ids move up)"}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in PROP_CHANGES.items()})

## Why the asset outputs changed for the OBJ tile heap (C2b: a sprite's tiles are checked against
## gfx::sprites::kMaxSpriteTiles, as the per-slot kTilesPerSlot is gone).
HEAP_REASON = "Each sprite's tile check names gfx::sprites::kMaxSpriteTiles (C2b: the OBJ tile heap)"
HEAP_CHANGES = {name: HEAP_REASON for name in ("sandbox_map/assets.cpp", "sandbox_menu/assets.cpp",
                                               "sandbox_screen/assets.cpp", "sandbox_sequence/assets.cpp",
                                               "starter_battle/assets.cpp", "test_assets.cpp",
                                               "test_script_assets.cpp")}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in HEAP_CHANGES.items()})

## Why the script test outputs changed for C3's menus (the menus script, its list and texts).
MENU_REASON = "The test scripts gain C3's menu statements (a list asset of three texts)"
MENU_CHANGES = {name: MENU_REASON for name in ("test_script.cpp", "test_script_assets.cpp", "test_script_ids.h")}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in MENU_CHANGES.items()})

## Why the script test outputs changed for C4's name slots (the names script, five str slots, a list, a name kind).
NAME_REASON = "The test scripts gain C4's name statements (five str name slots, a list asset, a game name kind)"
NAME_CHANGES = {name: NAME_REASON for name in ("test_script.cpp", "test_script_assets.cpp", "test_script_ids.h")}
## Every scripts' ids header names the Name op's kinds (C4: NAME_KIND_ITEM ...).
NAME_KIND_REASON = "script_ids.h names the Name op's kinds (C4: NAME_KIND_...)"
NAME_CHANGES.update({name: NAME_KIND_REASON for name in ("combine_script_ids.h", "field_script_ids.h",
                                                         "sandbox_map/script_ids.h", "steps_script_ids.h",
                                                         "test_map_script_ids.h", "test_script/script_ids.h")})
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in NAME_CHANGES.items()})

## Why the script test outputs changed for C5's counter boxes (the counters script, two counters).
COUNTER_REASON = "The test scripts gain C5's counter statements (two counter boxes)"
COUNTER_CHANGES = {name: COUNTER_REASON for name in ("test_script.cpp", "test_script_ids.h")}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in COUNTER_CHANGES.items()})

## The platform the battle and transfer compiles are checked for, as CMake passes CAPSULE_PLATFORM to each of them
## (the host build whose ctest runs this test; the outputs do not depend on it, the limit checks do).
SNAPSHOT_PLATFORM = "headless"
## The --platform option of those runs.
PLATFORM_ARGS = ["--platform", SNAPSHOT_PLATFORM]

## One compiler run: the tool, its arguments ({out} stands for the output folder) and the outputs it writes.
Run = tuple[str, list[str], list[str]]

## Why outputs changed when internal codes became plain words: the generated comments name what they describe
## (no package or decision codes), and the script fixture's port and field action scripts have plain names.
PLAIN_NAMES = "Generated comments without internal codes"
PLAIN_NAMES_SCRIPTS = (PLAIN_NAMES + "; the test scripts ports, port_other, port_loop, port_resume, port_warp_into and "
                       "field_action_escape (were i4, i4_*, f2_escape) and their SCRIPT_ ids")
PLAIN_NAMES_CHANGES = {name: PLAIN_NAMES_SCRIPTS if name.startswith("test_script") else PLAIN_NAMES
                       for name in (
                          "battle_conditions/battle_ids.h", "battle_conditions/test_battle.cpp",
                          "battle_gaps/battle_ids.h", "battle_gaps/test_battle.cpp", "battle_moves/battle_ids.h",
                          "battle_moves/test_battle.cpp", "battle_passives/battle_ids.h",
                          "battle_passives/test_battle.cpp", "battle_presenters/battle_ids.h",
                          "battle_presenters/test_battle.cpp", "battle_progress/battle_ids.h",
                          "battle_progress/test_battle.cpp", "battle_records/battle_ids.h",
                          "battle_records/test_battle.cpp", "battle_resources/battle_ids.h",
                          "battle_resources/test_battle.cpp", "battle_slots2/battle_ids.h",
                          "battle_slots2/test_battle.cpp", "battle_stats3/battle_ids.h",
                          "battle_stats3/test_battle.cpp", "battle_tiers/battle_ids.h",
                          "battle_tiers/test_battle.cpp", "battle_turn/battle_ids.h", "battle_turn/test_battle.cpp",
                          "battle_vm/battle_ids.h", "battle_vm/test_battle.cpp", "combine/battle_ids.h",
                          "combine/test_battle.cpp", "field/battle_ids.h", "field/test_battle.cpp",
                          "growth/battle_ids.h", "growth/test_battle.cpp", "inventory/battle_ids.h",
                          "inventory/test_battle.cpp", "recruit/battle_ids.h", "recruit/test_battle.cpp",
                          "sandbox_map/battle.cpp", "sandbox_map/battle_ids.h", "sandbox_map/encounter_ids.h",
                          "sandbox_map/world.cpp", "sandbox_party/battle.cpp", "sandbox_party/battle_ids.h",
                          "sandbox_screen/encounter_ids.h", "starter/battle_ids.h", "starter/test_battle.cpp",
                          "starter_battle/battle.cpp", "starter_battle/battle_ids.h", "steps/battle_ids.h",
                          "steps/test_battle.cpp", "test_battle/battle.cpp", "test_battle/battle_ids.h",
                          "test_encounter_ids.h", "test_script.cpp", "test_script_ids.h", "test_world.cpp")}
RECORDED_CHANGES.update({name: (RECORDED_CHANGES[name] + "; then " if name in RECORDED_CHANGES else "") + why
                         for name, why in PLAIN_NAMES_CHANGES.items()})

def sequence_symbol(name: str) -> str:
   """Symbol CMakeLists.txt gives a test sequence: kSeq + the CamelCased file name.
   @param name  file name without .json
   @return the symbol
   """
   return "kSeq" + "".join(part[:1].upper() + part[1:] for part in name.split("_"))


def engine_runs() -> list[Run]:
   """The compiler runs of the engine library and its tests.
   @return runs
   """
   t = ROOT / "tests" / "data"
   runs = [("font_compiler.py", [str(ROOT / "assets" / "fonts" / "default_8x8.txt"), "{out}/default_font.cpp",
                                 "kDefaultFont"], ["default_font.cpp"])]
   for target, name, symbol in INPUT_MAPS:
      runs.append(("input_compiler.py", [str(ROOT / "assets" / "input" / "default_input.json"), target,
                                         "{out}/" + name, symbol], [name]))
   for name in TEST_SEQUENCES:
      runs.append(("sequence_compiler.py", [str(t / "sequences" / f"{name}.json"), "{out}/seq_" + name + ".inc",
                                            "--symbol", sequence_symbol(name)], [f"seq_{name}.inc"]))
   runs += [
      ("asset_compiler.py", [str(t / "assets" / "test_assets.json"), "{out}/test_assets.cpp",
                             "--depfile", "{out}/test_assets.d"], ["test_assets.cpp"]),
      ("map_compiler.py", [str(t / "maps" / "world.json"), "{out}/test_world.cpp", "--depfile", "{out}/test_world.d",
                           "--ids", "{out}/test_encounter_ids.h", "--world-ids", "{out}/test_world_ids.h",
                           "--scripts", str(t / "maps" / "scripts" / "scripts.json"), "--debug-names"],
       ["test_world.cpp", "test_encounter_ids.h", "test_world_ids.h"]),
      ("script_compiler.py", [str(t / "maps" / "scripts" / "scripts.json"), "{out}/test_map_scripts.cpp",
                              "--ids", "{out}/test_map_script_ids.h", "--depfile", "{out}/test_map_scripts.d"],
       ["test_map_scripts.cpp", "test_map_script_ids.h"]),
   ]
   # the other battle data sets of the battle tests (cmake/parts/battle_tests.cmake)
   for folder, manifest in BATTLE_TEST_SETS + STEP_TEST_SETS + COMBINE_TEST_SETS:
      runs.append(("battle_compiler.py", [str(t / manifest), "{out}/" + folder + "/test_battle.cpp",
                                          "--ids", "{out}/" + folder + "/battle_ids.h",
                                          "--depfile", "{out}/" + folder + "/test_battle.d"] + PLATFORM_ARGS,
                   [f"{folder}/test_battle.cpp", f"{folder}/battle_ids.h"]))
   folder, manifest = STARTER_SET
   runs.append(("battle_compiler.py", [str(ROOT / manifest), "{out}/" + folder + "/test_battle.cpp",
                                       "--ids", "{out}/" + folder + "/battle_ids.h",
                                       "--depfile", "{out}/" + folder + "/test_battle.d"] + PLATFORM_ARGS,
                [f"{folder}/test_battle.cpp", f"{folder}/battle_ids.h"]))
   runs += [
      # the effect banks of the VM tests (cmake/parts/effect.cmake)
      ("effect_compiler.py", [str(t / "effects" / "vm.json"), "--emit", "{out}/test_effects_vm.cpp",
                              "--namespace", "vm_effects"], ["test_effects_vm.cpp"]),
      ("script_compiler.py", [str(t / "scripts" / "scripts.json"), "{out}/test_script.cpp",
                              "--ids", "{out}/test_script_ids.h", "--depfile", "{out}/test_script.d", "--debug-names"],
       ["test_script.cpp", "test_script_ids.h"]),
      # F1: the field fixture's scripts (its timed field effects' implicit variables and expiry scripts)
      ("script_compiler.py", [str(t / "field" / "scripts.json"), "{out}/field_script.cpp",
                              "--ids", "{out}/field_script_ids.h", "--depfile", "{out}/field_script.d"],
       ["field_script.cpp", "field_script_ids.h"]),
      # The step effects fixture's scripts (its counters, member and outcome variables, the rot's script)
      ("script_compiler.py", [str(t / "steps" / "scripts.json"), "{out}/steps_script.cpp",
                              "--ids", "{out}/steps_script_ids.h", "--depfile", "{out}/steps_script.d"],
       ["steps_script.cpp", "steps_script_ids.h"]),
      # The combination fixture's scripts (its counters, egg_member, the hatch script, the nursery statements)
      ("script_compiler.py", [str(t / "combine" / "scripts.json"), "{out}/combine_script.cpp",
                              "--ids", "{out}/combine_script_ids.h", "--depfile", "{out}/combine_script.d"],
       ["combine_script.cpp", "combine_script_ids.h"]),
      ("asset_compiler.py", [str(t / "scripts" / "assets.json"), "{out}/test_script_assets.cpp"],
       ["test_script_assets.cpp"]),
      # the record transfer tables and test vectors (cmake/parts/transfer.cmake)
      ("record_transfer.py", ["emit", str(ROOT / "samples" / "starter" / "battle.json"),
                              "{out}/transfer/test_transfer.cpp"]
       + PLATFORM_ARGS, ["transfer/test_transfer.cpp"]),
      ("record_transfer.py", ["cases", str(t / "transfer" / "cases.json"), "{out}/transfer/transfer_cases.h"]
       + PLATFORM_ARGS, ["transfer/transfer_cases.h"]),
   ]
   for name in TRANSFER_FIXTURES:
      runs.append(("record_transfer.py", ["emit", str(t / "transfer" / f"{name}.json"),
                                          "{out}/transfer/transfer_" + name + ".cpp", "--namespace", name,
                                          "--header", "{out}/transfer/transfer_" + name + ".h"] + PLATFORM_ARGS,
                   [f"transfer/transfer_{name}.cpp", f"transfer/transfer_{name}.h"]))
   return runs


def game_runs(app: Path) -> list[Run]:
   """The compiler runs of one app, as capsule_game() reads its reference/project.json.
   @param app  the app's folder
   @return runs; outputs are named <app>/<file>
   """
   project = json.loads((app / "reference" / "project.json").read_text(encoding="utf-8"))
   name = project["name"]
   gen = "{out}/" + name
   runs = []
   page = []   # --code-page PATH when the game sets a language
   language = project.get("text", {}).get("language")
   if language:
      page = ["--code-page", str(app / project["text"]["code_pages"][language])]
      fonts = project["text"].get("fonts", {}).get(language, [str(ROOT / "assets" / "fonts" / "default_8x8.txt")])
      fonts = [str(app / f) for f in ([fonts] if isinstance(fonts, str) else fonts)]
      more = [w for f in fonts[1:] for w in ("--glyphs", f)]
      runs.append(("font_compiler.py", [fonts[0], f"{gen}/font.cpp", "kDefaultFont"] + more + page,
                   [f"{name}/font.cpp"]))
   field = ["--scripts", str(app / project["scripts"])] if project.get("scripts") else []   # field text
   if project.get("assets"):
      runs.append(("asset_compiler.py", [str(app / project["assets"]), f"{gen}/assets.cpp",
                                         "--depfile", f"{gen}/assets.d"] + page + field, [f"{name}/assets.cpp"]))
   if project.get("maps"):
      args = [str(app / project["maps"]), f"{gen}/world.cpp", "--depfile", f"{gen}/world.d",
              "--ids", f"{gen}/encounter_ids.h", "--world-ids", f"{gen}/world_ids.h"]
      if project.get("debug_names"):
         args.append("--debug-names")
      if project.get("scripts"):
         args += ["--scripts", str(app / project["scripts"])]
      runs.append(("map_compiler.py", args + page,
                   [f"{name}/world.cpp", f"{name}/encounter_ids.h", f"{name}/world_ids.h"]))
   if project.get("battle"):
      runs.append(("battle_compiler.py", [str(app / project["battle"]), f"{gen}/battle.cpp",
                                          "--ids", f"{gen}/battle_ids.h", "--depfile", f"{gen}/battle.d"]
                   + PLATFORM_ARGS + page + field, [f"{name}/battle.cpp", f"{name}/battle_ids.h"]))
      runs.append(("record_transfer.py", ["emit", str(app / project["battle"]), f"{gen}/transfer.cpp"]
                   + PLATFORM_ARGS, [f"{name}/transfer.cpp"]))   # every game with battle data has its kTransfer
   if project.get("scripts"):
      args = [str(app / project["scripts"]), f"{gen}/scripts.cpp", "--ids", f"{gen}/script_ids.h",
              "--depfile", f"{gen}/scripts.d"]
      if project.get("debug_names"):
         args.append("--debug-names")
      runs.append(("script_compiler.py", args + page, [f"{name}/scripts.cpp", f"{name}/script_ids.h"]))
   for symbol, seq in project.get("sequences", {}).items():
      stem = Path(seq).stem
      runs.append(("sequence_compiler.py", [str((app / seq).resolve()), f"{gen}/seq_{stem}.inc", "--symbol", symbol]
                   + page, [f"{name}/seq_{stem}.inc"]))
   return runs


def all_runs() -> list[Run]:
   """Every compiler run of a build with every app.
   @return runs
   """
   runs = engine_runs()
   for project in sorted((ROOT / "apps").glob("*/reference/project.json")):
      runs += game_runs(project.parents[1])
   return runs


def generate(out: Path) -> dict[str, str]:
   """Runs every compiler into a folder.
   @param out  output folder
   @return {output name: sha256}
   """
   hashes = {}
   for tool, args, outputs in all_runs():
      argv = [sys.executable, str(TOOLS / tool)] + [a.replace("{out}", str(out)) for a in args]
      for o in outputs:
         (out / o).parent.mkdir(parents=True, exist_ok=True)
      r = subprocess.run(argv, capture_output=True, text=True, cwd=out)
      if r.returncode != 0:
         raise RuntimeError(f"{' '.join(argv)} failed:\n{r.stderr}")
      for o in outputs:
         hashes[o] = hashlib.sha256((out / o).read_bytes()).hexdigest()
   return hashes


class GeneratedSnapshot(unittest.TestCase):
   """The compilers write exactly the recorded outputs."""

   def test_outputs_match_the_snapshot(self) -> None:
      """Every output has its recorded hash; no output is missing or new."""
      recorded = json.loads(HASHES.read_text(encoding="utf-8"))["outputs"]
      with tempfile.TemporaryDirectory() as tmp:
         hashes = generate(Path(tmp))
      self.assertEqual(sorted(hashes), sorted(recorded), "outputs added or removed")
      changed = sorted(o for o in hashes if hashes[o] != recorded[o])
      self.assertEqual(changed, [], "outputs differ from the snapshot (record intended changes, see the docstring)")

   def test_recorded_changes_name_real_outputs(self) -> None:
      """RECORDED_CHANGES only names outputs the snapshot holds."""
      recorded = json.loads(HASHES.read_text(encoding="utf-8"))["outputs"]
      self.assertEqual([o for o in RECORDED_CHANGES if o not in recorded], [])


def record() -> None:
   """Rewrites hashes.json from the current compilers.
   @return None
   """
   with tempfile.TemporaryDirectory() as tmp:
      hashes = generate(Path(tmp))
   HASHES.parent.mkdir(parents=True, exist_ok=True)
   data = {"schema": "capsule.snapshot/1", "outputs": dict(sorted(hashes.items()))}
   HASHES.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
   print(f"{HASHES}: {len(hashes)} outputs recorded")


if __name__ == "__main__":
   if "--record" in sys.argv:
      record()
   else:
      unittest.main()
