# Battle

The battle engine, its data and rulesets, the Starter sample's rules, the bag, item use, the save image and growth.

## Battles

`capsule::battle` (`include/capsule/battle/battle.h`) runs battles; what a
battle does that is content is data (`include/capsule/battle/data.h`), and
the formulas behind stats, experience, rewards and escape and the pipelines
of damage, speed, accuracy and critical hits are the game's ruleset
(`include/capsule/battle/ruleset.h`, `pipeline.h`). Following the spec
(C-08, D-18, D-20, D-22, D-23, P-13, P-18):

- **Generic core (P-13).** The engine names no franchise terms: it knows
  parties, members, presenters, traits, passives, categories and rewards;
  a game's words for them are its manifest's `terms` (the Starter sample
  says Rival, Temperament, Talent, Charm, Coins) and its message texts. A
  `Participant` brings party data (`PartyData`: members, presenters, AI
  flags, rank, `PARTY_NO_ESCAPE`), records (`monster::Party`), or both;
  `PARTY_UNOWNED` parties (creatures met without an owner) announce each
  creature (`appeared`), need a position per creature, are never replaced
  and are not written back. `can_escape()` refuses running when the setup
  has `SETUP_NO_ESCAPE` or a party on another side has `PARTY_NO_ESCAPE`.
  `tools/ci/check_principles.sh` rejects the franchise words in the battle,
  monster and text core, the battle bench, the tools and the neutral battle
  data (tests/data/battle_*, tests/data/effects, tests/bench, samples).
- **Messages.** The engine says the messages of `battlec.texts`
  `REQUIRED_MESSAGES` itself; every manifest gives them. The others
  (`OPTIONAL_MESSAGES`: super_effective, not_very_effective, hit_count, the
  confusion / flinch / protect texts, recoil, drained, healed, hp_full) are
  only named by data and logic (chart and tier messages, condition texts,
  effect `say`): a manifest may leave them out, and then cannot name them
  (the Starter leaves out hit_count, protected, protect_start and recoil).
- **Passive slots** (D-25 decision 6): structure `passive_slots` lists the
  slots per species, `[{"name", "rolled"}]` (default one rolled slot, at
  most 4 on the GBA and 8 elsewhere). A species names up to one passive per
  slot (an empty slot gives the first slot's passive), a member its slot by
  index or name, and the generation entry `passive_selector` `[0, "last"]`
  rolls among the rolled slots up to the species' last filled one. A slot
  that is not rolled (`"rolled": false`) is given only by data or logic: the
  classic sample's `hidden` slot is its manifest's choice, not the engine's
  shape. The PassiveSelector role keeps the slot in its low bits (enough for
  the slot count; the compiler checks the width).
- **Presenters** (who shows a party on screen): each party names up to two.
  A battle opens with `Phase::Intro`, one party per step: its presenters
  enter (`EventKind::Presenter` cues), speak their intro
  (`EventKind::PresenterText`; without one a party given as data says
  `party_challenge`), and those that leave after the intro go; the rest
  leave right before the party's first send-out unless they stay. When a
  party is defeated its returning presenters come back and say their defeat
  text; the winners say their victory text. The engine only queues cues;
  `battle::presenter_view` (`presenter_view.h`) plays them with sprites,
  the video blend and the sequence player (slide, fade, sequence, stay).
- **One State, driven in steps.** `start()` copies the parties in and
  refuses setups that could never finish (a party with nothing to send out
  or no position, an unowned party larger than its positions). Each
  `step()` runs one small unit of work (one party's intro, the opening send-outs, one battler's
  switch-in effects, one target's accuracy check, one effect or one hit of a
  move, one experience award, one level-up, one battler's end-of-turn
  effects, one replacement) and records what happened as presentation events
  in a 256-entry ring (text, HP, status, stages, faints, switches, weather,
  experience, levels). A move runs to its end before anything else; its
  experience is shown after it. The presentation plays events at its own
  pace; the engine never waits for it. `request()` / `choose()` take human
  choices (move and target, switch, run, replacements) and refuse invalid
  ones.
- **Formats are data.** Positions, sides, reach between positions (`all`,
  or `adjacent` for triples) and the screen layout; the Starter has 1 vs 1
  and 2 vs 2. A side's positions are split between its parties;
  `stands` say where each side's presenters stand.
- **The turn structure is data** (D-22 A11, E6b; `turn`,
  `capsule::battle::TurnRules`): the order brackets of running, switching,
  items and moves (`"move": "priority"` uses each move's priority, a number
  puts every move in one bracket; then speed, then the `tie` rule:
  `random_byte`, a byte drawn per battler each turn, or `position`), the
  end-of-turn phases in order (`weather`: countdown and residual damage;
  `battlers`: each battler's residual hooks, optionally only some sources
  and in their own order; `sides` / `field`: each condition's residual for
  each of its battlers or `"each": "once"`, then its countdown; `regen`: the
  resources' regeneration; `delayed`, E13b: each position's pending effect of
  effect `delay` counts down and lands, needed by logic that uses `delay`), the hook
  dispatch order of the sources (`effect_order`: status, volatile, side,
  field, weather, passive, item, each once) and the battle start
  (`switching`: send-outs foes first or in position order, switch-in hooks
  by speed or in send-out order). Every key is optional; the defaults are
  the engine's former fixed order, which the sample writes out.
- **Logic is effect code** (D-22 decision 1, D-23 section 1;
  `include/capsule/battle/effect.h`): every move, passive, held item,
  status, volatile, weather, side and field condition is an *owner* of one
  bytecode bank
  (`BattleData::effects`, compiled by `tools/effect_compiler.py`) with
  handlers for engine hooks (`switch_in`, `switch_out`, `faint`,
  `turn_start`, `before_move`, `used_move`, `before_accuracy`, `try_hit`,
  `before_damage`, `damaging_hit`, `after_hit`, `after_move`, `set_status`,
  `stage_change`, `update`, `residual`, `order`, the condition-own
  `try_start` / `start` / `already` / `end`, `side_residual`,
  `field_residual`; E13b: `try_switch`, which may refuse a battler's chosen
  switch or run (trapping; a forced switch does not ask; C5: the battler's
  own owners, then the `per_foe` handlers of each active foe's owners, so a
  foe can trap it; `is_trapped()` asks without texts, `choose()` says why
  once a turn), and `redirect`, which may retarget another battler's chosen
  single-target move (C5: `selected` / `selected_foe` moves, a target
  phase's single target too; value 255 in, `modify set BATTLER` claims it,
  the first valid claim in position order wins) and move phases
  (`use`, `target`, `try_hit`, `hit`, `after_move`; E13b: `land`, a pending
  effect's arrival). E13b's primitives: `stage ... silent` (no text),
  `reset_stages WHO` (every stage to 0, no texts, no stage_change hooks),
  `delay WHO amount turns` (the move's land phase on that position later),
  `reward amount` (the reward pool), `end_battle` (the battle ends, nobody
  wins; C5: the unit stops there and the experience earned is paid before the
  End event); refs `targeted_by` (the last move that reached a battler past
  protection and accuracy since it entered), `delayed_turns`,
  `human`, `unowned`, `gene(STAT)` and `species_field(NAME)`; the `crit` flag
  is set before the `before_damage` hooks run, and a typed `damage_hp` runs
  them too (C5: without the flag). C5: the type a move hits a target with
  (the `move_type` point) is worked out once per target and is what hooks,
  phases and point handlers read as `move.type`; an unstored record role
  cannot be written. The move skeleton stays engine code (before_move, costs,
  "used", use, targets, per target before_accuracy / accuracy / try_hit /
  hit, after_move); a phase that yields (each hit of a `repeat`ed `damage`)
  or asks (`ask`, `switch ... pass`) is suspended in `State::mv.run` and
  goes on in the next step, a human's answer coming through `request()`
  (`SwitchTarget`, `LearnMove`, `Choice`) and `choose()` with
  `ActionKind::Answer`. Hooks run the holder's owners in the manifest's
  dispatch order (`turn.effect_order`; the sample: its status, its
  volatiles, its side's and the field's conditions, the weather, then its
  passive and held item); a
  battler never re-enters a hook it runs (a berry fires once), and
  past `kMaxEffectDepth` nested runs a dispatch is skipped and
  `State::vm.faults` counts it. `src/battle/effect_host.cpp` implements the
  VM's host functions (reads, writes, record roles, draws on the battle
  stream, the primitives). Logic comes from the `.ceffect` sources the
  manifest names (`"effects": ["effects/moves.ceffect", ...]`, with owner
  `"texts"`, `"cues"` and `"tags"`); the manifest keeps only data. A
  damaging move without logic hits each target once; `on switch_in per_foe`
  marks a switch-in handler that also runs once per foe on the field. The
  legacy effect lists, hook handlers and modify fields (before E4c) are
  unknown keys. Point handlers (below) are part of their owner.
- **Stats are data** (D-20 C): the manifest's `stats` list names each
  permanent stat, its in-game label and the ruleset stat formula that
  computes it (`rules::stat(index, ...)`); one is the pool stat (hit points,
  `max_hp()`), one the order stat (turn order and escape), and at most one
  the odds stat (`"odds": true`, E9: the ruleset's optional `effect_odds`
  pipeline reads it for both battlers when effect logic rolls `odds(n, m[,
  by, on])`; see ["Starting a new game from the Starter sample"](#starting-a-new-game-from-the-starter-sample)). Up to 10 stats on
  the GBA and 32 elsewhere (`kMaxStats`; the compiler checks the target's
  limits, `--platform`); arrays hold `kMaxStats` and the count is read at run
  time. Traits may change the stats with the `trait` flag.
- **Stages are data** (D-22 A7, E6a): the manifest's `stages` records, in
  stage order (`STAGE_<NAME>` ids): a stat's stage, accuracy, evasion or the
  critical-hit stage, each with its label, its range (within +-15) and its
  table (`[num, den]` per stage; odds for the crit stage) or a ruleset
  formula of (value, stage). Stage changes clamp to the stage's range; the
  pipelines read them through `stage_scale(id, v, stage)` (the inputs
  `offense_stage_id`, `defense_stage_id`, `speed_stage_id`), `staged()` (the
  table every stat stage shares), `accuracy_staged()` and `crit_chance()` /
  `crit_out_of()`.
- **The type chart's tiers are data** (D-22 A6, E6a): `chart.tiers` (up to
  8: a Q8 factor, or decisive `zero` / `ko` with a message), the combine mode
  of a defender's types (`exponent`: the pipelines' `type_exp` with
  `pow2()`; `multiply`: one combined factor; `sequential`: each factor in
  turn; `type_scale(v)` applies whichever), `messages.above` / `below`,
  `types_per_species` and `added_types` (at most 3 types per battler), and
  the matchups (tier names or multipliers; the rest take `default`).
- **Categories are data**: up to four, each naming the attacker's and the
  defender's stat by name (none: a non-damaging category); damage and the
  self-hit (`rules.self_hit.category`) take their stats from it.
- **Pipelines** (D-20 D, P-14): damage, a self-hit (effect `self_hit`),
  speed, accuracy and critical hits are the ruleset's pipelines, lists of
  steps (`let` a value with an optional clamp, run a modifier `point` on it,
  `roll` a draw at exactly that position) compiled to straight-line C++
  (`rules::damage()` ... `rules::crit()`, `include/capsule/battle/pipeline.h`).
  The engine gathers the inputs (stats, stages, crit, spread, same type,
  type exponent, ...) and uses the outputs; the type chart's decisive tiers
  end a matchup before the pipeline, the first in type order winning (C-08h). The Starter's
  ruleset writes out its own steps (critical, same type, type chart, spread,
  weather, a 92-108 % roll last, status, passive / item) with Q8
  truncations and one draw (["Starter rules"](#starter-rules)).
- **Points**: a point (`offense`, `power`, `weather`, `damage`, `speed` in
  the sample) names its sources in order (user / target passive, item,
  status; the weather; E13b: the move's own handlers, each side condition on
  a battler's side and each volatile it has); `battle::run_point()` runs their
  handlers in that order, each seeing the value clamped to 16 bits, then the
  point's range clamps it; a point nobody handles costs a mask test. A point
  named `move_type` (E13b) is run by the engine on a hit's type before the
  chart, the same-type check and `type_scale()`; point handlers then read the
  type the hit has as `move.type` (hooks still read the move's own). Point handlers are
  the pure part of the effect language (`point power when move.type ==
  "Flame" { modify by 1.2; }`: reads, arithmetic, `if`, `modify by | add |
  set`; no writes, events or draws, so previews never draw), compiled to one
  generated `rules::point_handler()` switch. Statuses and weathers carry
  point handlers too (Scorch's damage, Shock's speed, Storm's type
  multipliers in the Starter), and so do moves (variable power, a type by
  weather or genes), volatiles and side conditions (screens; E13b).
- **Conditions are records** (D-22 A3-A5, D-23 section 2): `statuses`
  (at most 63), `volatiles` (32: a bit per battler, with up to 8 counters
  and 4 words in all, cleared at the end of the turn, on switch-out or on
  faint as the record says, handed on by a passing switch), `weathers`,
  `side_conditions` (turns and layers per side) and `field_conditions`
  (turns), plus `move_flags` (16; `move.has(flag)`) and `tags` (16). A
  record's declarative fields are templates the battle compiler expands
  into effect code in front of the condition's own `.ceffect` handlers
  (`tools/battlec/conditions.py`): residual damage (optionally escalating),
  lost turns (odds or a length), cure odds, thawing by a move flag or a
  type, immune types, durations, chains (`chain {den, cap, kept_by}`) and
  texts; the engine keeps only names, texts, owners and the volatiles'
  slots and clear rules. Each battler also keeps the last damage it took
  from a move (`taken.*`). The Starter's flinch and daze are such records.
- **AI (D-18)** decides at the start of each turn from that moment's state,
  before any human choice exists, with one draw for the turn that every
  estimate's damage roll scales;
  flags pick expected damage, avoiding moves with no effect, and status moves.
- **Progression is data** (D-22 A8 / A12, E6c; `capsule::battle::ProgressRules`).
  Experience is the ruleset's `exp_gain` (with the foe's party rank as
  `foe_rank`, clamped to `rules.rank_max`), shared by `exp_policy`:
  `share` `faced` (battlers that were on the field with the foe), `active`
  or `party`, `receivers` `human_side` or `opposing_sides` (AI parties too),
  `outsider` `never` or `owner_id` (1 for records whose owner_id is not
  `Setup::owner_id`). Level-ups recompute stats and learn the level's moves
  into free slots; with every slot taken `learn.on_full` skips the move,
  replaces the oldest (`replace_oldest`) or asks (`request`: step() returns
  Input, request() gives `RequestKind::LearnMove`, `State::learn.move` is the
  move, the answer is the slot to forget or `kKeepMoves`; AI parties keep
  their moves). `learn.default_moves` (`last_n_by_level` or `first_n`) picks
  the moves of creatures built without any. A move resource's maximum (PP)
  is its `max` formula of the move's base and the slot's raises (see
  Resources). `make_monster()` runs the
  `generation` list in order, which is its draw order: `random_range`
  ([lo, hi]; `"max"` = the role's width; `"last"` = the last trait or the
  species' last rolled passive slot; one draw up to 65536 values, two for a
  power of two above, none for a single value), `constant`, or `derived`
  (an expression over the record's single-element roles, 32-bit ones as
  `NAME_hi` / `NAME_lo`, generated as `rules::derive()`; a role the layout
  does not store is read through `battle::record_value()`). The defaults
  are neutral (D-22 decision 8): `learn.default_moves` and
  `exp_policy.share` have none and must be given, `learn.on_full` is
  `skip`, `exp_policy` gains go to the human side without an outsider bonus,
  and without a `generation` list nothing is drawn (every role keeps its
  default), then fills every persisted resource. The sample writes out the
  engine's former fixed rules (tests/data/battle_progress shows the others).
- **Resources** (D-25, `include/capsule/battle/data/resources.h`,
  `tools/battlec/resources.py`): every pool a battler spends or loses is a
  structure record of `resources`, in the order costs are paid. A `battler`
  resource has one pool per battler with a stat as its maximum; exactly one
  has `"empty": "faint"` on the pool stat: the hit points (`Battler::hp`).
  A `move` resource has one pool per move slot (`Battler::move_pool`): its
  `base` names the move field every move states (e.g. `pp`), its `max` a
  ruleset formula of (base, raises) (default the base) and `raises` the
  record role of each slot's raises. `persist` names the record role a pool
  is kept in between battles (else it is refilled every battle); the role
  must hold the pool's highest value (a battler pool's stat's analysed
  maximum, a move pool's max formula range or largest base), so a
  write-back never truncates a pool (the engine clamps and asserts too).
  `regen` (a number within int16, or a formula of (value, max)) runs in the
  turn's `regen` end-of-turn phase. A move's `cost` is
  `{resource: amount or formula}` (default: 1 of every move resource); a
  move whose cost cannot be paid is not selectable (player and AI; the AI's
  `save_resources` flag also prefers cheaper moves); a number cost no pool
  can ever reach is refused. A cost on the hit points needs more hit points
  than it takes: paying never faints the user (the move is not usable
  instead). `rules.fallback_move` runs, paying nothing and stating no cost,
  when no move can be paid. Effect logic reads and writes
  `holder.resource(NAME)` / `resource_max(NAME)` and `move.base(NAME)` /
  `move.cost(NAME)`, and has `restore WHO NAME [amount] [every_slot]` and
  `raise WHO NAME delta [every_slot]`. Battlers hold 4 battler resources
  (hit points included) and 2 move resources on the GBA, 8 and 4 elsewhere.
  ```json
  "resources": [{"name": "hp", "label": "HP", "scope": "battler", "stat": "hp", "empty": "faint",
                 "persist": "current_hp"},
                {"name": "pp", "label": "PP", "scope": "move", "base": "pp", "max": "pp_max", "raises": "pp_ups",
                 "persist": "pp"},
                {"name": "ep", "label": "EP", "scope": "battler", "stat": "ep", "regen": "ep_regen"}]
  ```
  `python3 tools/battle_resources.py path/to/battle.json` migrates a battle
  from before E8 (the hit points and PP as resources, the ruleset's `pp_max`
  slot as a named formula and the pp resource's max whenever the ruleset
  has it, `move_slots` 4, the message `no_pp` as `no_usable_move` (dropped
  when both are given), `lock ... no_pp` as `lock ... free`); it writes a
  battle's files all or none.
- **Rewards**: a victory adds the ruleset's `reward(reward_base, level)` for
  each defeated party given as data (its `reward` and its last member's
  level) to the pool effect logic may have added to (`reward`, E13b) and shows
  one `reward` message.
- **Deterministic.** All randomness comes from the battle's own stream,
  seeded in the setup (A-4): the same seed, parties and choices replay the
  same battle (the basis for link lockstep and recorded battles).
- At the end, human parties' records get their persisted resources (HP,
  PP) and raises (0 for a move learned in battle), status, experience,
  level, moves and held item back.

Deliberate differences from the spec: a battler slot is larger than the
spec's 60 B (it carries what level-ups need, so the battle never reads the
records or copies ROM tables until it ends); `kMaxPositions` is 6 (REF-17).

Not in this milestone: point handlers of field conditions (no point source
yet), global owners, AI scripts, battle animations,
link and recorded battles, SOS calls, parties joining, Battle Royal scoring,
stackable rule modules (REF-15), bosses, capture, the Bag, the move-forget
prompt, held-item effects beyond the hooks above.

### Creature records

`capsule::monster` (`include/capsule/monster/monster.h`): engine code reads
records by **role**. Engine roles come from a frozen, append-only registry
(species key, seed, owner ID, experience, level, moves, PP, PP raises, genes,
training, awakening, trait, passive selector, held item, current HP, status,
nickname, text language); IDs never move. PP, PP raises and current HP are
plain storage since E8: the battle reads them only where a resource names
them as its `persist` or `raises` role. A game adds **custom roles** (D-22
decision 2: gender, met data, ribbons, friendship, ...): fields the engine
stores and scripts read and write, but no engine code interprets. The game
picks the roles in its battle data's `monster_layout`; the battle compiler
packs them (bytes and larger fields byte-aligned) and emits the layout table
(offset, element width and count per engine role, the custom role table)
and its hash:

```json
"monster_layout": {"party_size": 6, "move_slots": 8,
                   "roles": ["species_key", "level", {"role": "pp", "width": 6}, {"role": "nickname", "count": 10},
                             {"custom": "gender", "width": 2}, {"custom": "ribbons", "width": 1, "count": 32},
                             {"custom": "friendship", "width": 8, "default": 50}, "..."]}
```

- A plain list is the same with 4 move slots and parties of 6 (record
  transfer fixtures); a battle's layout states both.
- A layout chooses an engine role's width up to the engine's maximum (the
  8- and 16-bit fields battlers keep, 32 for seed, owner ID, experience;
  species key and status are fixed) and the nickname's length (1-12); the
  compiler checks the data fits (32 moves need 6-bit moves: all bits set is
  "no move"). The per-stat roles hold one element per stat, moves / PP / PP
  raises one per move slot (`move_slots` 1-8; battlers hold 8 and the engine
  loops over the layout's). `party_size` is up to 6 on the GBA and 8
  elsewhere; party storage is that cap, formats' `party_max` and parties are
  checked against the layout.
- Custom roles (at most 32) have IDs 0x80 + their index; `battle_ids.h`
  names every role `ROLE_<NAME>`, `monster::find_custom(name_hash("gender"))`
  finds one by name, `monster::reset()` gives a new record their defaults
  (`make_monster()` does). Owner secret, owner name, friendship, form,
  capture device and "nicknamed" are custom roles now (A10): the registry
  keeps their IDs reserved, and a bare old name still compiles as the custom
  role of its old shape. A non-empty nickname names a battler.
- The hash folds the engine role IDs in order, a chosen width or count only
  where it differs from the registry, each custom role as (name hash, width,
  count) — one with a retired role's name and old shape folds that role's ID
  — and the party size where it is not 6; defaults are not folded. The
  sample keeps hash 0x62409274 and its 60-byte record.

A record is at most 100 bytes on the GBA and 256 elsewhere. A role a layout
leaves out reads as 0.

#### Record transfer (v2)

`include/capsule/monster/transfer.h` moves a record between games whose
layouts differ (trades, links, a save carried to another game).
`export_record()` writes the **canonical transfer record**, keyed by role and
stat instead of bit offset: a version byte, the engine role count and a
presence mask over the engine role IDs, the custom role keys and the stat
table (sorted 32-bit keys: FNV-1a of `"role:" + name` and of `"stat:" +` the
stat's `transfer` key), each present role's width and count, then the values
bit-packed at those widths (per-stat roles in stat table order, move roles
by slot). It is deterministic and layout-independent: the same values in
another role order give the same bytes. `import_record()` copies matching
roles and stats into the receiver's layout; values too wide are clamped to
the receiver's maximum (flag `kTransferNarrowed`; "no move" and the end code
stay all-ones), incoming roles, stats or non-empty elements without a place
are dropped (`kTransferDropped`), and the receiver's own get their defaults
(custom roles' `default`, "no move", end codes, 0, or the layout's
`transfer_defaults`; `kTransferDefaulted`). Games with the same transfer hash
(layout hash and stat keys) can send the raw record (`form_for()`). Nothing
allocates; `transfer_size()` / `kMaxTransferBytes` size the buffer.

```json
"monster_layout": {"party_size": 6, "roles": ["..."], "transfer_defaults": {"spa": {"stat_genes": 7}, "spd": 3}}
```

A game's table (`capsule::monster::kTransfer` for its `kLayout`) comes from
`capsule_transfer_table(battle.json out.cpp)` (cmake/parts/transfer.cmake;
`tools/record_transfer.py emit`); capsule_game() generates it for every game
with battle data (`generated/<name>/transfer.cpp`, linked only when the game
transfers records). The battle compiler checks the stat transfer keys too. `tools/record_transfer.py` is the same
format in Python (record dicts or bytes; `export` / `import` on the command
line) and is checked byte for byte against the C++ (tests/transfer_tests.cpp,
tests/data/transfer). Move indices, species keys and statuses are copied as
numbers: games with different tables remap them themselves.

### Battle data and rulesets

A game lists its battle data in `reference/project.json` (`"battle":
"data/battle.json"`). `tools/battle_compiler.py` compiles it with the
ruleset it names into `battle::kData`, the formula functions and the record
layout, plus `battle_ids.h` (constexpr indices: `SPECIES_...`, `MOVE_...`,
`PARTY_...`, `PRESENTER_...`, `CATEGORY_...`, `FORMAT_...`, `STAT_...`,
`STAGE_...`, `TIER_...`, `RESOURCE_...`, `ROLE_...`) for game code. CMake passes `--platform` so the data is checked against that
platform's limits (`tools/build_settings.py` `PLATFORM_LIMITS`).
The battle data is two manifests (D-23 section 2, E5s): `battle.json`
(`capsule.battle/1`) holds the content and names the ruleset and the
structure manifest; `structure.json` (`capsule.structure/1`, next to it) holds
the game's battle structure. Each file may only hold its own keys (the
compiler names the right file for a key in the wrong one; the lists are
`tools/battlec/manifest.py` `BATTLE_KEYS` / `STRUCTURE_KEYS`). A single-file
manifest from before the split is refused with the command that migrates
it, `python3 tools/battle_split.py path/to/battle.json` (it writes
`structure.json`, or `<name>.structure.json` for another file name, and
moves the structure keys there unchanged, so the compiled data stays the
same). The compiler's docstring describes every key; in short:

```json
{"schema": "capsule.battle/1", "ruleset": "ruleset.json", "structure": "structure.json",
 "types": [{"name": "Fire"}, {"name": "Cosmic"}],
 "moves": [{"name": "Fireball", "type": "Fire", "category": "elemental", "power": 70, "accuracy": 100,
            "cost": {"mp": 8}, "target": "selected"},
           {"name": "Strike", "type": "Cosmic", "category": "physical", "power": 20, "cost": {}}],
 "passives": [{"name": "Blaze Heart", "handlers": [{"point": "power", "code":
   "when move.type == Fire and holder.max_hp > 0 and holder.hp * 5 <= holder.max_hp * 2 { modify by 1.4; }"}]}],
 "species": [{"name": "Emberkit", "types": ["Fire"], "base": [45, 40, 50, 62, 58, 44, 42, 48, 61],
              "passives": ["Blaze Heart"], "growth": "steady", "exp_yield": 62,
              "learnset": [[1, "Strike"], [5, "Fireball"]]}],
 "traits": [{"name": "Even"}, {"name": "Fierce", "up": "attack", "down": "protection", "pct": [115, 88]}],
 "presenters": [{"name": "Coach", "sprite": "sprite:PRESENTER", "enter": {"style": "slide", "frames": 24},
                 "exit": {"style": "slide", "frames": 16}, "intro": "{party} wants to battle!"}],
 "parties": [{"name": "Novice Kit", "presenters": ["Coach"], "reward": 20, "rank": 1, "escape": false,
              "ai": ["damage"], "members": [{"species": "Emberkit", "level": 6}]}],
 "messages": {"used_move": "{user} used {move}!", "...": "..."},
 "effects": ["effects/moves.ceffect"]}
```

```json
{"schema": "capsule.structure/1",
 "monster_layout": {"party_size": 6, "move_slots": 8, "roles": ["species_key", "level", "..."]},
 "stats": [{"name": "hp", "label": "HP", "formula": "vital", "pool": true},
           {"name": "mp", "label": "MP", "formula": "reserve", "trait": false}, "...",
           {"name": "attack", "label": "Attack", "formula": "core"}, "...",
           {"name": "agility", "label": "Agility", "formula": "core", "order": true}],
 "resources": [{"name": "hp", "scope": "battler", "stat": "hp", "empty": "faint", "persist": "current_hp"},
               {"name": "mp", "scope": "battler", "stat": "mp", "persist": "current_mp"},
               {"name": "ep", "scope": "battler", "stat": "ep", "persist": "current_ep", "regen": "ep_regen"}],
 "stages": [{"name": "accuracy", "kind": "accuracy", "range": [-4, 4], "table": [[60, 100], "...", [140, 100]]},
            {"name": "evasion", "kind": "evasion", "label": "evasion", "range": [-4, 4]},
            {"name": "critical", "kind": "crit", "label": "focus", "range": [0, 3],
             "table": ["1/20", "1/10", "1/5", "3/10"]},
            {"stat": "attack", "range": [-4, 4], "table": [[40, 100], "...", [100, 100], "...", [200, 100]]},
            "..."],
 "chart": {"tiers": [{"name": "none", "decisive": "zero", "message": "no_effect"}, {"name": "weak", "q8": 154},
                     {"name": "normal", "q8": 256}, {"name": "strong", "q8": 448}],
           "combine": "multiply", "messages": {"above": "super_effective", "below": "not_very_effective"},
           "types_per_species": 2, "added_types": 1, "default": "normal",
           "matchups": {"Fire": {"Bio": "strong", "Fire": "weak"}}},
 "categories": [{"name": "physical", "offense": "attack", "defense": "defence"},
                {"name": "elemental", "offense": "magic", "defense": "protection"}, {"name": "status"}],
 "growth": {"brisk": "6*n*n + 24*n", "steady": "8*n*n + 32*n"},
 "learn": {"default_moves": "last_n_by_level"}, "exp_policy": {"share": "faced"},
 "formats": [{"name": "Single", "sides": [[0], [1]], "range": "all", "layout": [[64, 92], [176, 52]]}],
 "turn": {"brackets": {"run": 30, "switch": 20, "item": 25, "move": "priority"}, "tie": "random_byte",
          "end_of_turn": [{"run": "weather"}, {"run": "battlers"}, {"run": "sides"}, {"run": "field"},
                          {"run": "regen"}],
          "effect_order": ["status", "volatile", "side", "field", "weather", "passive", "item"]},
 "terms": {"party": "Rival", "passive": {"one": "Talent", "many": "Talents"}}}
```

A structure whose types decide physical or special (an older generation's
rule) states `"category_by_type": {"<type>": "<damaging category>", ...}`
for every type (E11): a damaging move then takes its type's category and
states none (or a non-damaging one, a status move); a move naming another
damaging category is refused (`tools/battlec/stats.py`
`compile_category_by_type()`). Its table also gives the category of a hit
whose type the ruleset's `move_type` point changed; a structure whose moves
state their categories can give that alone (C5): `"hit_category_by_type":
{"<type>": "<damaging category>", ...}` (every type) makes a damaging move
whose hit type is not its own type hit with that type's category (stats,
`move.category`, `taken.category`; e.g. a move whose type is chosen when it is used); both keys at
once are refused.

The structure manifest also holds `rules`, the condition records
(`statuses`, `volatiles`, `weathers`, `side_conditions`,
`field_conditions`), `move_flags`, `tags` and `generation`; the content
manifest also holds `items` and the names logic uses (`texts`, `cues`).

**Neutral defaults** (D-22 decision 8): a key a manifest may leave out
defaults to an identity or the engine's limit, never to a game's numbers:
`rules.stat_cap` 65535, `rules.level_max` 255, `rules.self_hit.power` 1, a
species' `exp_yield` 0, an escalating status's cap 255, no generation list,
the `turn` structure of the engine's former order. Some keys have no default
at all: `monster_layout.party_size`, a trait's `pct` when it names `up` or
`down`, `learn.default_moves`, `exp_policy.share`, `monster_layout.move_slots`
and, since E8, the resources: the faint resource (hit points) must be
declared, every move states each move resource's base, and a move resource
without `max` has its base as maximum. The pool stat stays a stat flag; the
faint resource names it.

A move's logic is `.ceffect` code in the sources `effects` lists (`move "Fireball" { hit { damage; if target.alive
and odds(10, 100) { inflict target Scorch quiet; } } }`; a damaging move without logic hits each target once).

Texts may write `{@concept}` or `{@concept:many}` (messages, status,
weather and presenter texts): the compiler expands the game's terms, so
nothing about terms reaches the ROM. A concept given only in its one form
gets its many form from the terms' plural rule (`"plural": "english"`, the
default: Ability -> Abilities, Status -> Statuses, EXP and Species stay;
`"same"`: the one form); `battle_compiler.plural_of()` is the rule the
studio shows. With the game's scripts manifest (`--scripts`, which
capsule_game() passes) the texts may also use its field placeholders
(`{player}`): `battle::format_text()` keeps them for the printer.
A presenter's entrance or exit may be a sequence (`capsule.sequence/1`, compiled into the battle data); it may
only move sprite slots 14-15 with sprite, wait, blend, sound and loop ops.

A ruleset (`capsule.ruleset/2`) gives the formula slots (`exp_gain`,
`reward`, `escape_odds`), named formulas (with declared `args`, called from
pipelines), the stat formulas (named; each stat names one), the points and
the five pipelines, as integer expressions (`+ - * / %`, comparisons, `min
max clamp abs isqrt if`; in pipelines also `staged`, `accuracy_staged`,
`crit_chance`, `crit_out_of` over the battle's tables, `mulq8`, `pow2`, and
`STAT_CAP` / `LEVEL_MAX` from the battle rules) with a declared range per
input and per result (the sample scales each stat by its own stage,
`stage_scale(offense_stage_id, offense_raw, offense_stage)`, and the type
chart by `type_scale(d)`, so it compiles with differing stat stages and in
every combine mode):

```json
{"schema": "capsule.ruleset/2",
 "constants": {"HP_FLAT": 10, "HP_START": 8, "HP_DIV": 22, "DAMAGE_LEVEL_BIAS": 5, "DEFENCE_PAD": 10,
               "DAMAGE_DIV": 50, "DAMAGE_FLAT": 1, "CRIT": 416, "ROLL_LOW": 92, "ROLL_SPAN": 17, "ROLL_DEN": 100},
 "inputs": {"level": [1, 50], "power": [1, 1023], "attack": [1, 700], "defense": [1, 700], "...": []},
 "formulas": {"damage_base": {"args": ["level", "power", "attack", "defense"],
   "expr": "attack * power * (level + DAMAGE_LEVEL_BIAS) / ((defense + DEFENCE_PAD) * DAMAGE_DIV) + DAMAGE_FLAT",
   "range": [1, 65535]}},
 "stat_formulas": {"vital": {"expr": "HP_FLAT + base / HP_START + level * (base + gene) / HP_DIV",
                             "range": [1, 700]}},
 "points": {"power": {"holder": "user", "sources": ["passive", "item"], "range": [1, 1023]}},
 "pipelines": {"damage": [{"let": "attack", "expr": "stage_scale(offense_stage_id, offense_raw, offense_stage)",
                           "range": [1, "STAT_CAP"]},
                          {"let": "defense",
                           "expr": "if(crit, defense_raw, stage_scale(defense_stage_id, defense_raw, defense_stage))",
                           "range": [1, "STAT_CAP"]},
                          {"let": "power", "expr": "move_power"}, {"point": "power", "on": "power"},
                          {"let": "d", "expr": "damage_base(level, power, attack, defense)", "range": [0, 65535]},
                          {"let": "d", "expr": "if(crit, mulq8(d, CRIT), d)", "range": [0, 65535]},
                          {"roll": "r", "span": "ROLL_SPAN"},
                          {"let": "damage", "expr": "max(d * (ROLL_LOW + r) / ROLL_DEN, 1)"}],
               "...": []}}
```

The compiler bounds every intermediate value over the input ranges and
fails the build if one can leave int32, a divisor's range holds 0, or isqrt
can see a negative value (P-18); the generated function clamps its result to
the declared range, and `ruleset_compiler.py <ruleset.json>` reports whether
the clamp can engage. The battle compiler then checks the ruleset against
the engine: every input range must cover the values the engine can pass
(genes, training and awakening up to what the layout's widths hold, levels
up to `level_max`, the data's base stats and yields, the speed pipeline's
speeds for `escape_odds`, ...), the stat formulas must declare ranges within
16 bits, and every pipeline is analysed over this battle's stats, moves and
tables (`ruleset_compiler.bind()`: a named formula's arguments must stay in
its declared ranges, outputs inside what the engine stores).
`ruleset_compiler.run_pipeline()` evaluates a pipeline in Python with a
point callback (the battle compiler's `point_runner()`, which runs the
handlers with `effect_compiler.run_pure`); the reference-ruleset repo's
pipeline tests reproduce every damage and preview grid
line of the WP-2 goldens with it.
It also keeps each step small: at most 8 effects per handler, 10 hits per
multi-hit move and 4 moves learned at one level, and each text may use only
the placeholders the engine fills for it (`{party}` and `{passive}` among them). Growth rates are expressions in `n` turned into tables
at build time (clamped, never decreasing). The engine itself contains no
game's formulas (P-13): the test and sandbox rulesets are invented.

## Starting a new game from the Starter sample

`samples/starter/` is the engine's neutral sample game (D-25, E9): a complete battle data set with invented
content that a new game copies and then changes, and the engine's neutral reference fixture (its goldens are
recorded with the engine's default random generator). The studio's "Starter" template copies the same files.

| File | What it holds |
| --- | --- |
| `structure.json` | nine stats (`hp`, `mp`, `ep`, `attack`, `magic`, `defence`, `protection`, `intelligence`, `agility`); the resources `hp` (faints when empty), `mp` and `ep` (persisted in the custom roles `current_mp` / `current_ep` like the hit points; `ep` regenerates in the turn's `regen` phase); the categories physical (Attack vs Defence), elemental (Magic vs Protection) and status; nine types in a tiered `multiply` chart; four statuses, two volatiles, one weather; 8 move slots, party size 6; the terms Rival, Temperament, Talent, Charm, Coins |
| `ruleset.json` | stat formulas `vital` (HP), `reserve` (MP, EP: their own bases and growth) and `core`; `ep_regen`; the damage, speed, accuracy and crit pipelines with points for talents and charms; the `effect_odds` pipeline; the Starter's own numbers ("Starter rules") |
| `battle.json` | the nine types, 28 moves (each type a physical move paying EP, an elemental move paying MP and a status move paying MP; `Strike`, the free and weak fallback move), nine talents (two latent slots per species), three charms, six temperaments, nine creatures (one per type), a presenter, three rivals and the engine messages |
| `effects/*.ceffect` | the moves', talents', charms' and conditions' logic (secondary effects and statuses roll `odds()`) |
| `assets.json` | placeholder art, simple shapes only: a front / back sprite per creature, the presenter, the backdrop |
| `README.md` | the Starter rules table (every number and formula) and how to change them |

**Intelligence and effect odds.** `stats[].odds: true` names the stat (here Intelligence) that the ruleset's
optional `effect_odds` pipeline reads for both battlers (`user_stat`, `target_stat`, besides `effect_chance`,
`effect_out_of` and both levels). Effect logic asks for odds with `odds(n, m)` (the user against the target) or
`odds(n, m, by, on)`; the Starter scales them by `(2 * user_stat + 60) / (user_stat + target_stat + 60)` (see
"Starter rules"), then runs the points
`odds` (the user's talent and charm: Insight, Focus Lens) and `resist` (the target's: Stubborn). A ruleset without
`effect_odds` gets the plain odds, drawn exactly like `chance(n, m)`. `battle::effect_odds_preview()` gives the
odds without drawing (AI, editors).

To start a game from it (the toolkit's **New game** with the Starter template does steps 1 and 2, and adds a world
and a first scene, `src/game.cpp`, so the game builds at once: ["Building a game"](BUILDING.md#building-a-game)):

1. Copy `samples/starter/` into the game, e.g. as `data/` beside `reference/project.json`.
2. In `project.json`, name the copies by relative path: `"battle": "data/battle.json"`, `"assets":
   "data/assets.json"` (see `apps/starter_battle/reference/project.json`, which names the sample in place).
3. Rename and change what the game needs: stats, resources and costs (`resources`, each move's `cost`), types and
   the chart, the creatures, moves and logic; replace the shape art. `python3 tools/battle_compiler.py
   data/battle.json out.cpp --platform gba` checks the data against the GBA's limits.
4. Keep scenarios for the bench (`tests/bench/starter/*.json` are examples: each reproduces a golden transcript)
   and record the game's own expectations once its rules settle.

The sample's own checks: `starter_golden_tests` (transcripts and the odds grid in `tests/data/starter/golden`,
plus the resource and odds rules), `starter_sample_tests` (`tests/test_starter_sample.py`: the sample's contract,
every platform) and `bench_starter_*` (the bench `capsule_starter_bench`, default generator).

## Starter rules

The Starter's numbers and formulas are its own (C2, the developer's decision of 2026-10-19): no stage table, odds,
factor or formula shape of another game. Every rule is data in `samples/starter/` (ruleset constants, structure
records, effect logic), so a new game changes them there; the engine only runs the pipelines, points and formulas.
`samples/starter/README.md` has the same table with notes on changing it; `starter_golden_tests` checks the main
numbers directly (caps, stage tables, crit odds and factor, chart tiers, the roll, regen, escape, experience,
reward, the odds bound).

| Rule | Number or formula | Where | Why |
| --- | --- | --- | --- |
| Level cap | 50 | `structure.json` `rules.level_max` | a short game; the growth curves stay small (16 200 / 21 600 EXP) |
| Stat cap | 700 | `rules.stat_cap` | just above the highest HP the formula reaches (654); every stat formula stays within it |
| Staged stats | up to 1400 (`STAGED_CAP`: the stat cap x the top stage's x2) | `ruleset.json` `constants`, the `attack` / `defense` / `speed` lets, the `offense` point, the inputs `attack`, `defense`, `speed`, `foe_speed` | a staged stat is not clamped at the stat cap, so +3 and +4 still count at the top end (C4) |
| Genes | 0..15 per stat | `stat_genes` role, 4 bits | unchanged record layout |
| HP (`vital`) | `10 + base / 8 + level * (base + gene) / 22` (10..654) | `ruleset.json` `stat_formulas` | a small head start from the base, then linear growth: low levels are not one-hit |
| MP, EP (`reserve`) | `6 + level * (base + gene) / 30` (6..456) | `stat_formulas` | pools sized for 2-4 casts at low levels; no temperament |
| Attack .. Agility (`core`) | `(3 + base / 16 + level * (base + gene) / 36) * temperament / 100` (2..451) | `stat_formulas` | the same shape as HP with a smaller head start |
| Temperaments | raised stat x1.15, lowered stat x0.88 | `battle.json` `traits[].pct` | a slight net gain, so no temperament is a pure loss |
| Damage | `Attack * power * (level + 5) / ((Defence + 10) * 50) + 1` | `formulas.damage_base` | attack against padded defence: a low Defence does not multiply damage without bound |
| Damage order | base, crit, same type, chart, spread, weather point, roll, damage / guard points, at least 1 | `pipelines.damage` | the roll comes last so every factor is rolled together |
| Damage roll | 17 outcomes, x0.92 .. x1.08 (`ROLL_LOW` 92, `ROLL_SPAN` 17, `ROLL_DEN` 100) | `pipelines.damage` | a narrow roll, centred on x1 |
| Critical hit | x1.625 (`CRIT` 416 / 256); uses the target's Defence without its stages, the user's stages apply | `pipelines.damage` | a crit pierces the target's guard; boosts the user earned still count |
| Crit odds | stage 0..3: 1/20, 1/10, 1/5, 3/10 | `structure.json` `stages` (critical) | rare by default, a real choice at the top stage |
| Same type | x1.2 (`SAME_TYPE` 307 / 256) | `pipelines.damage` | a small bonus, so off-type moves stay useful |
| Spread | x0.7 (`SPREAD` 180 / 256) when a move hits more than one target | `pipelines.damage` | |
| Type chart | strong x1.75 (448), weak x0.6 (154), none x0 (decisive, "no effect"), `multiply` | `structure.json` `chart.tiers` | double strong x3.06, strong and weak x1.05, double weak x0.36 |
| Stat stages | -4..+4: x0.40, 0.55, 0.70, 0.85, 1, 1.25, 1.50, 1.75, 2.00 | `stages` (one shared table) | linear both ways: +25 % a stage up, -15 % a stage down |
| Accuracy | -4..+4: x0.6 .. x1.4, 10 % a stage, of accuracy stage minus evasion stage (clamped) | `stages` (accuracy, evasion) | |
| Self-hit | power 30, physical, the damage formula and roll without crit or chart | `rules.self_hit`, `pipelines.self_hit` | |
| Dazed | 1-3 moves; each move, 1 in 4 hits itself (power 30) | `volatiles`, `effects/conditions.ceffect` | |
| Messages | the engine's required keys, the optional `super_effective`, `not_very_effective`, `drained`, `healed`, `hp_full`, and its own `dazed`, `daze_hit`, `daze_end`, `became_dazed`, `already_dazed`, `staggered` | `battle.json` `messages` | a battle declares the messages its logic says itself (C4): neutral keys, no borrowed ones |
| Effect odds | `chance * (2 * user Int + 60) / (user Int + target Int + 60)`, then the `odds` / `resist` points | `pipelines.effect_odds` | equal Intelligence keeps the odds asked for; a clever user at most doubles them, a clever target drives them toward 0 |
| EP regen | `1 + (EP max - EP) / 16` each `regen` phase; MP does not regenerate | `formulas.ep_regen`, `resources` | small, and faster when the pool is nearly empty |
| Growth | brisk `6*n*n + 24*n`, steady `8*n*n + 32*n` (EXP to reach level n) | `structure.json` `growth` | quadratic: later levels cost more, but not steeply |
| Experience | `max(1, yield * foe level / 10 * (foe level + 10) / (own level + 10) * (4 + rank) / 4 / participants)` | `formulas.exp_gain` | the level difference counts: a stronger foe gives more, a weaker one less; a rival x1.25 |
| Reward | `base * (level + 8) / 2` (level: the rival's last creature) | `formulas.reward` | |
| Escape | out of 256: `96 + 128 * speed / (speed + foe speed) + 48 * failed attempts` | `formulas.escape_odds` | a share of the combined speed: never hopeless, never certain at first |
| Scorch | 1/12 of max HP a turn (21 / 256); the scorched battler's physical damage x0.7 | `statuses`, `conditions.ceffect` | |
| Blight | 1/10 of max HP a turn (26 / 256) | `statuses` | |
| Shock | loses 1 move in 5; speed x0.7 | `statuses`, `conditions.ceffect` | |
| Doze | 1-3 moves | `statuses` | |
| Storm | 5 turns; Lightning x1.3, Fire x0.7 | `weathers`, `conditions.ceffect` | |
| Talents | Blaze Heart Fire x1.4 at 2/5 HP or less; Quick Feet speed x1.3 at half HP or less; Thick Hide physical damage taken x0.75; Insight odds x1.5; Stubborn odds against it x0.5 | `effects/passives.ceffect` | |
| Charms | Focus Lens odds x1.25; Mana Drop +20 MP and Vigor Bar +20 EP at a quarter or less | `effects/items.ceffect` | |
| Bag (G2) | 4 pockets: Supplies 20 slots of up to 99 (as added), Orbs 8 of 99 (by index), Charms 16 of 9 (by name), Key 16 of 1 | `structure.json` `pockets` | the D-26 decision 2 pockets; the GBA holds 256 slots, the Starter uses 60 |
| Supplies | Tonic +20 HP (out of battle and, from G3, in battle), Mana Tea +15 MP, Spark Snack +15 EP, Remedy cures the status; a full pool, a fainted member or no status: no effect, nothing used | `battle.json` `items`, `effects/items.ceffect` (`use_field`) | small, cheap restores; the charms stay held items |
| Prices and shops | Tonic 30, Mana Tea 40, Spark Snack 25, Remedy 50, Orb 20 (its recruit use is G3's), Mana Drop / Vigor Bar 120, Focus Lens 200; Gate Pass is a key item; Corner Shop (supplies, Orb), Charm Stall (charms) | `items[].price`, `shops` | |
| AI | Scholar Wen adds `save_resources` (prefers cheaper moves); the other rivals do not | `battle.json` `parties[].ai` | the clever rival spends its MP carefully, the novice does not |
| Field rules (F1) | the lead is the first member with HP (no `lead` rule); the Ward charm (Charms pocket, 300 coins, used up) starts the timed field effect `ward`: 120 steps, started over when used again, survives a save, "The Ward charm's glow fades." when it ends; while it runs nothing below the lead's level is met; a lead with Quick Feet meets creatures half as often again (encounter rate x3/2); the encounter pipelines are the engine's defaults | `structure.json` `field_effects`, `battle.json` `items`, `effects/field.ceffect`, `effects/passives.ceffect` | a ward that is neither all nor nothing (strong creatures still come), a talent that only matters when it leads |

The pipeline has no target level, so the level difference enters damage only through the user's `level + 5` and
both battlers' stats; experience compares the two levels directly. All the arithmetic stays inside the ruleset
compiler's interval analysis (int32 everywhere; `damage_base` reaches 143 221 with a staged Attack of 1400 against
Defence 1, and the pipeline clamps it to 65 535).

## Inventory, item use and the save image (G2, D-26 section 2)

**Pockets and item fields** are manifest data (`tools/battlec/inventory.py`,
emitted as `capsule::inventory::kInventory` beside the battle tables;
`battle_ids.h` names `POCKET_<NAME>` and `SHOP_<NAME>`):

```json
"pockets": [{"name": "supplies", "label": "Supplies", "capacity": 20, "quantity_max": 99, "sort": "added"},
            {"name": "key", "label": "Key", "capacity": 16, "quantity_max": 1}]
"items": [{"name": "Tonic", "pocket": "supplies", "price": 30, "use": "both", "target": "member"},
          {"name": "Gate Pass", "pocket": "key", "key": true}]
"shops": [{"name": "Corner Shop", "items": ["Tonic", "Mana Tea"]}]
```

- `pockets` (structure.json): each a slice of the bag's slots (`capacity`
  distinct items of up to `quantity_max`, default 99), ordered by `sort`
  (`added`, `index`: item id, `name`: display name). At most 8 pockets and
  256 slots on the GBA (1 KB of slots), 16 and 1024 elsewhere
  (`build_settings` PLATFORM_LIMITS `max_pockets` / `max_bag_slots`).
- Item fields (battle.json): `pocket` (none: the item cannot go into the bag),
  `price`, `use` (`none` | `field` | `battle` | `both`), `target` (`none` |
  `member` | `move_slot` | `battler`, battler only for use `battle`), `key`
  (never consumed or sold), `consumed` and `holdable` (both default to not
  key). A use needs its effect logic hook (`on use_field` / `on use_battle`)
  and a hook of a use the item does not have is refused. A party member
  holds only holdable items. `shops`: what the script statement `shop`
  offers, at the items' prices (each needs a price and a pocket).

**The bag** (`capsule/inventory/bag.h`, `src/inventory/bag.cpp`): fixed
`kMaxBagSlots` storage laid out at run time by `kInventory`; `add()` /
`remove()` are all or nothing (no pocket, a full pocket, past quantity_max),
`count()`, `room_for()`, `sort()` / `sort_all()`, `valid()`. No heap.

**Item use out of battle** (`capsule/inventory/use.h`): a `FieldUse` scope
opens a field context (`capsule/battle/field_context.h`: the party member's
record as battler 0 of a one-battler State the game lends, its held item and
passive left out) and runs the item's `use_field` handlers with the battle's
own effect host, so `restore`, `raise` (PP-Up style through the resource's
`raises` role), `cure`, `text` and `say` work as in battle; `fail` is "no
effect" and nothing is consumed. The new primitive `teach WHO MOVE` offers a
move as a level-up does: into a free slot at once, else a move offer the
game answers (`UseResult::LearnOffer`, then `answer_learn(slot)`; the item
is consumed only when learned; the default learn question is
`script::items::learn_open()`); it fails when the move is known. The scope's
end writes the member back (persisted resources and raises, status, moves).
`use_battle` compiles now; the battle's Item action runs it from G3 on.
Evolution items run `transform` (G4, below).

**Scripts**: `give_item("Tonic", 2) -> var(ok);`, `take_item(...)`,
`count_item(...) -> var(n);` (an item name of the manifest's `battle`, or an
operand holding its id), `open_bag -> var(item);`, `shop "Corner Shop" money
var(coins);` and `time_of_day -> var(t);` (statements, not keywords: older
scripts may use the words). They go through `capsule::script::items` in
`capsule/script/port.h`; the engine's defaults (`src/script/script_items.cpp`)
use the bag given to `items::attach()` and the text box's choice menu.

**The save image** (`capsule/save/save.h`, `src/save/save.cpp`; D-26
decision 1): one engine image whose sections are sized from the manifests:
the bag (4 bytes per manifest slot, 2 per pocket), the party (a count, then
party_size records), the storage boxes (0 bytes until G3) and the script
banks (flags, variables, name slots). A 32-byte versioned header with the
record layout's hash and a shape hash (an image of another shape is
refused), one FNV-1a checksum over the whole image, two copies when two fit
(a write goes to the older one). `save::write()`, `save::read()` (nothing in
RAM changes unless the checksum holds) and `save::load()` (read, then the
script VM sees the new flags). The medium is `capsule/hal/save.h`: the GBA
cartridge's 64 KB flash (`src/hal/gba/save_gba.cpp`, or 32 KB SRAM with
`CAPSULE_SAVE_SRAM`), a desktop file `<game>.sav` in the working folder or
`$CAPSULE_SAVE_DIR` (`src/hal/sdl2/save_file.cpp`, compiled per game), RAM
elsewhere. `tools/save_layout.py` works out the same sizes at build time:
`capsule_game()` writes `generated/<game>/save_layout.txt` and fails the
build when the image is over the platform's budget (`max_save_bytes`: the
GBA's 64 KB); its `image()` writes the same bytes as the engine (the studio's
test saves; `tests/data/inventory/save_image.bin` is checked against both).

**Time of day** (`capsule/core/clock.h`, D-26 decision 3): a context value
the game supplies by defining `clock::time_of_day()` (the default,
`src/core/clock_default.cpp`, answers 0), so carts without a clock work.

**Sandbox**: `capsule_sandbox_party` (`apps/sandbox_party`): the Starter's
party and bag (pools and status per member, the four pockets, item use with
member and move-slot choice and a learn question, the shops, the save image
on the platform's medium). L / R change page; the Save page writes (A), reads
(START) and resets (SELECT).

## Growth: transformations, forms and training values (G4, D-26 section 4)

Every rule, number and name is manifest data (`tools/battlec/growth.py`,
emitted as `capsule::growth::kGrowth` with the generated
`rules::growth_when()` and `rules::train_gain()`; `battle_ids.h` names
`TRIGGER_<NAME>`, `CONTEXT_<NAME>` (and `CONTEXT_<NAME>_<VALUE>`) and
`FORM_<NAME>`):

```json
"transformation": {"context_values": [{"name": "time_of_day", "values": ["day", "night"]},
                                      {"name": "game_version", "values": ["standard", "attack"]}],
                   "cancel": ["level_up"], "blocked": "held_item == still_stone && trigger != item_use",
                   "form_role": "form"},
"training": {"gain": "ev_gain", "stat_cap": 255, "total_cap": 510, "yields": {"hp": "ev_hp", "atk": "ev_atk"}}
"species": [{"name": "Puddlekit", "transforms": [
               {"into": "Puddlecrown", "when": "friendship >= 220 && time_of_day == day", "on": ["level_up"]},
               {"into": "Puddlemonk", "when": "friendship >= 220 && time_of_day == night", "on": ["level_up"]}]},
            {"name": "Strataknight", "forms": [
               {"name": "Attack", "base": [50, 180, 20, 180, 20, 150], "when": "game_version == attack"},
               {"name": "base", "when": "game_version == standard"}]}]
```

- **Rules** (`species[].transforms`: `into`, `when`, `on`, `takes_item`,
  `also`): the first whose triggers include the trigger and whose `when`
  holds wins. `when` is the ruleset expression language over the record's
  single-element roles (stored or unstored, through `battle::record_value()`;
  32-bit ones as `NAME_hi` / `NAME_lo`), per-stat roles as `ROLE.STAT`, the
  permanent stats as `stat.NAME`, the species' fields as `species.NAME`,
  `trigger`, `item` (item_use: the item used; trade: the held item; else 0;
  items by their lower-case identifier or `item.NAME`) and the context values
  (their value names are words). Triggers: `level_up`, `item_use`, `trade`,
  `after_battle`, `script`; without `on` a rule fires on every trigger its
  expression can hold for (worked out by the interval analysis; a rule that
  can never hold is an error). `takes_item` uses the held item up (a
  trade-with-item rule); `also` names a species made beside it into a free
  party slot (a copy without held item, nickname or status).
- **Context values**: `time_of_day` is the game's clock port
  (`capsule/core/clock.h`); every other one is the game's port function
  `growth::context_value(index)` (default: its manifest `default`;
  `src/growth/growth_port_default.cpp`, replaced at link time). Values are
  clamped to their declared ranges.
- **Cancelling**: by data (`blocked`: while it holds nothing changes) and by
  the player for the triggers `cancel` names (never `item_use`: the item's
  logic transforms or fails).
- **Forms** (`species[].forms`: `name`, `types`, `base`, `passives`, `front`,
  `back`, `when`, `on`; an entry named `base` with only `when` / `on` is the
  rule back to the species' own form): form 0 is the species itself; each
  form is a full species view the battle reads through
  `growth::species_view()`. The record's form is the custom role
  `transformation.form_role` (stored for forms chosen by rules; it may be
  derived and unstored for forms only data gives). In battle the effect
  primitive `form WHO NAME` changes a battler's form (types, passive when the
  form gives others, stats re-derived): battle only, the record keeps its
  own; in a field context (an item's use_field) the record takes it.
- **Training values** (`training`): each battler exp_policy shares a defeated
  foe's experience with gains, stat by stat, `rules::train_gain(yield, value,
  total, level)` of the foe species' yield field (a stat whose yield is 0
  gains nothing), capped by the total and then the stat cap; written back
  with the record; the battle's stats follow at the next level-up, a
  record's at once. Items change them with `train WHO STAT AMOUNT` (the caps
  apply; negative lowers).

**Trigger points**: once a battle is over the game looks at each member
(`growth::levelled_members(state, party)` says whose battlers levelled up:
their trigger is `level_up`, the others' `after_battle`); an item's
`use_field` logic runs `transform holder;` (the item_use rules with this
item; none holds: the logic fails, nothing is used up); `trade` after a
transfer import with the held item as the context's item; the script
statement `transform(<member>) -> var(v);` (0 nothing, 1 changed, 2 stopped)
through `capsule/script/growth_port.h` (defaults: a Change over the State
given to `script::growth::attach()`, the choice menu as the question, the
items port's learn question).

**A Change** (`capsule/growth/growth.h`) is a scope over a field context in
a State the game lends: it finds the rule (`Stage::Asking`, a
`EventKind::Transform` event `kTransformStart`), `go()` applies it (species,
its base form, types, passive, stats and resource maxima re-derived: the
hit points and pools gain what their maxima gained; the stored derived roles
that read the species or form re-derived, the others kept) and offers the
new species' moves of its level as a level-up does (`Stage::Learning`,
`answer_learn()`), `stop()` stops a cancellable one; the scope's end writes
the member back. `growth::find()` only looks.

**Sandbox**: `capsule_sandbox_party`'s Growth page (a level up with its
transformation, the after-battle form by the time of day, Glow Charm's form
toggle, Drill Notes' training) and Bloom Charm on the Bag page.
