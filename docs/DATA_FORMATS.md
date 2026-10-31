# Data and manifest formats

The manifests a game's data is written in: sprite frames, worlds, maps, tilesets, encounters, field rules, map events, objects and locations.

## Sprite frames from images

Besides shape lists, a sprite frame can be a PNG file (path relative to the
manifest, exactly the sprite's size). `"palette": "auto"` picks up to 15
colors from the sprite's images (exact when they use 15 or fewer, a
median-cut reduction otherwise); with an explicit palette, pixels map to the
nearest entry. Alpha below 128 is transparent. An animation either shows
every frame for `"period"` display frames or gives one value per frame in
`"durations"`:

```json
{"key": "sprite:BALL", "kind": "sprite", "width": 32, "height": 32, "palette": "auto",
 "frames": [{"image": "frames/ball/ball_0.png"}, {"image": "frames/ball/ball_1.png"}],
 "animations": [{"frames": [0, 1, 0], "durations": [6, 12, 6]}]}
```

The compiler writes a dependency file, so editing an image rebuilds the
assets. PNGs are read with the engine's own reader (no imaging library); it
takes 8-bit and palette images, not 16-bit or interlaced ones. The scene
editor in capsule-studio writes frames in this form.

## Overworld maps

`capsule::map` (`include/capsule/map/map.h`) holds compiled maps and draws
them; `capsule::field` (`include/capsule/field/field.h`) moves a walker on
them. Both follow the design spec (C-03a/b/c, A-9, E-7):

- **Layers and cells (C-01b).** A map has up to four tile layers, each on
  its own background (BG0-BG3, chosen per map) with its own draw priority,
  plus one attribute layer. A metatile is 16x16 pixels of graphics only (one
  layer of 2x2 tiles) and can be painted on any tile layer, the way RPG
  Maker XP layers work; a chunk is 8x8 metatiles. A map is a sparse set of
  chunks; per cell it stores one metatile id per tile layer and a 32-bit
  attribute cell: behavior, environment tag, collision, elevation, the
  encounter flag and the encounter slot. Outside the chunks, and on
  unpainted cells, a border pattern (one per tile layer) shows; it is
  always impassable and anchored to world coordinates.
- **Draw order.** Lower priority numbers are in front; on a tie the lower
  BG is. Sprites take a priority per elevation (the world's `elevations`)
  and cover tile layers of the same or a higher priority number, so
  a canopy layer at a lower number covers the walker. Only tile layers
  exist so far; parallax and other layer contents come later.
- **Terrain classes are data.** The world manifest names them and says what
  each does: solid, passable only by some vehicle classes (water: swim),
  slide (ice), ledge (jump in one direction), push (forced step).
- **Rendering.** The map owns the backgrounds its layers name, character
  blocks 0-1 (1,024 tiles) and palette banks 0-12; block 2 and banks 13-15
  stay free for text and HUDs. While a map is shown, `gfx::background` runs
  the **map plan**: the map's BGs are reserved, and BG0, when the map leaves
  it free, moves to character block 2 and screen block 31, so text and the
  engine's text box (and `script_port_hal.cpp`) show over the map. Text
  styles whose bank the map uses (maps with 13 palettes) print in style 0's
  bank. The text box borrowing a map layer (C-01c) comes later. Each layer's
  32x32 screen is a ring around the camera; moving redraws
  only the metatile rows and columns that came into view. Tile animations
  swap runs of tiles in VRAM on their period, staggered by the compiler.
- **Movement.** Cell by cell at the speeds of the walker's vehicle class
  (on foot by default walking 1 and running 2 pixels per frame); a new
  direction turns first. Collision, elevation and terrain decide each step;
  ledges jump two cells with an arc.
- **Field rules are data (D-22 decision 5).** The world's `elevations` give
  each of the 16 levels the levels it meets (`"all"`, `"same"` or a list;
  two levels meet when either names the other), what arriving does
  (`"take"` the level, or `"keep"` the mover's, as on bridges) and a sprite
  priority. Without them the default rule applies: 0 meets every level
  (stairs), 15 meets every level and keeps the mover's (bridges), 1-14 meet
  themselves. The world's `vehicle_classes` are movement modes: walking and
  running speeds, whether the class may run, the sprite animation its set
  starts at and the behaviors it may enter. `field::set_vehicle()` and the
  script statement `vehicle NAME;` change the walker's class; a map with
  `"can_run": false` forbids running. The compiler's
  `compile_elevations()`, `default_elevations()` and
  `compile_vehicle_classes()` work in-process for editors.

One deliberate difference from the spec: the chunk directory entry is 20
bytes with natural alignment (it also points at the chunk's tile layers)
instead of a packed record, so the GBA reads it with aligned loads.

Not in this milestone: map connections,
per-chunk palette and tile streaming (C-02c; maps use fixed palettes for
now), ZX0 compression. Encounters: see Encounters below.

### World, tileset and map manifests

A game lists its world in `reference/project.json` (`"maps":
"data/world.json"`, plus `"debug_names": true` for sandboxes);
`tools/map_compiler.py` compiles it at build time with a dependency file, so
editing a tileset, map or sheet image rebuilds the tables.

```json
{"schema": "capsule.world/1",
 "vehicle_classes": ["foot", {"name": "swim", "speeds": {"walk": 2, "run": 2}, "can_run": false,
                              "first_animation": 12, "passable": ["water"]}],
 "behaviors": [{"name": "normal"}, {"name": "water", "passable": ["swim"]},
               {"name": "ice", "slide": true}, {"name": "ledge_down", "ledge": "down"}],
 "elevations": [{"name": "any", "meets": "all"}, {"name": "low"}, ...,
                {"name": "bridge", "meets": "all", "on_arrive": "keep", "priority": 2}],
 "tilesets": {"outdoor": "tilesets/outdoor.json"}, "maps": {"meadow": "maps/meadow.json"},
 "start": {"map": "meadow", "x": 16, "y": 11, "facing": "down"}}
```

The world may also organise its maps in `"map_groups"`, a tree of groups
(`{"name", "maps", "groups"}`, each map in at most one group) that editors
show as folders. The compiler checks it; the runtime does not use it yet.

Tilesets (`capsule.tileset/2`) have sheets of 8x8 tiles (drawn from shapes,
or PNG images with their palette picked automatically and an optional
transparent color), metatiles (`{"name", "tiles": [4 tile references]}`,
references are `"sheet:index"` with `h` / `v` flips) and animations. Maps
(`capsule.map/2`) list the tilesets they draw from (`"tilesets"`), their
tile layers (`{"bg", "priority", "purpose"}`), a dictionary of the
`"tileset/metatile"` names they use, a dictionary of the world behaviors
their cells use, the border (`"size"` and one pattern per layer) and the
chunks: `"cells"` are 8x8 attribute cells (`null` = unpainted, or `behavior
| env << 8 | collision << 16 | elevation << 18 | encounter << 22 | slot <<
23`) and `"layers"` holds 8x8 dictionary indices (or `null`) per tile layer.
A map may be blank (no metatiles, chunks or border; blank cells show the
backdrop and are impassable). The compiler deduplicates tiles with flips and
checks the limits (1,024 tiles, 13 palettes, 4,095 metatiles, 32 tile
animations and 256 behaviors per map). The map and tileset editors in
capsule-studio read and write these files.

Imported graphics (I2) whose tilemap entries already name VRAM tiles and
palette banks use **fixed-placement tilesets**: `"place": {"sheet", "tile",
"tiles", "palette", "palettes"}` loads the sheet's tiles at VRAM tile
`tile` and the tileset's `"palettes"` (16 colors each) at bank `palette`;
its sheets are indexed PNGs read as they are (`"indexed": true`), its
metatiles are raw entries (`{"name", "entries": [4 x tile | flips |
bank << 12]}`) and its animations name `"at"` (a tile of its place). A map
drawing from fixed tilesets loads all of its `"tilesets"` at their places
(no renumbering, places may not overlap); `"metatiles": "all"` is every
metatile of the map's tilesets in order. Any animation may give a
`"phase"` (frames into its period at map entry); a behavior may give
`"walls"`, the sides of its cell a step may not leave or enter through.
Maps sharing tilesets or cells share their compiled arrays.

### Encounters

`capsule::encounter` (`include/capsule/encounter/encounter.h`, ROM layout in
`data.h`; design C-03c/d/e, D-12) rolls encounters from tables the map
compiler builds (`tools/encounter_compiler.py`, run as part of it):

- **Slots.** An encounter cell (the encounter flag) also has a **slot**
  (0-15, bits 23-26 of the attribute cell); a map has one table per slot
  and **method**.
- **Methods are data.** The world names them (`"encounter_methods"`): step
  methods roll when the walker arrives on an encounter cell while in one of
  their vehicle classes (walking, swimming); script methods (one per tool or
  interaction, ...) roll when game code calls `encounter::roll_at()`.
  Default: one step method, `step`.
- **Tables** are weighted pools (C-03d): a rate (P-5 odds that a trigger
  starts an encounter) and up to 64 entries, each a species (from the
  battle manifest the world names, `"battle"`), a level range, a weight,
  the **conditions** it requires or excludes and weight multipliers while
  conditions are active (Q8, truncating). Condition names come from the
  world (`"encounter_conditions"`, up to 32); game code sets their bits
  with `encounter::set_conditions()`.
- **Sharing (C-03e).** The world may name a file of shared tables
  (`"encounter_tables"`) that maps use by name; identical tables are stored
  once in the global bank, whoever wrote them.
- **Field.** Arriving on an encounter cell rolls the first step method that
  applies (on `rng::gameplay`); when an encounter starts, the walker stops
  there (no slide or push) until the game takes it with
  `field::take_encounter()`. `field::set_encounters(false)` turns rolls off.
- `encounter_ids.h` (generated next to the world) names the methods and
  conditions for game code: `ENCOUNTER_METHOD_SURF`,
  `ENCOUNTER_CONDITION_NIGHT`, ...

```json
{"schema": "capsule.encounters/1",
 "tables": [
   {"slot": 0, "method": "walk", "rate": "1/10", "entries": [
     {"species": "Sproutle", "levels": [3, 5], "weight": 40, "excludes": ["night"]},
     {"species": "Duskowl", "levels": 4, "weight": 30, "requires": ["night"]},
     {"species": "Voltmink", "levels": [4, 6], "weight": 10, "modifiers": [{"when": "rain", "multiply": 3}]}]},
   {"slot": 1, "method": "swim", "table": "pond"}]}
```

A map names its file with `"encounters"` (relative to the map file).
`tools/encounter_compiler.py <world.json>` checks a world's encounters on
its own; capsule-studio's encounter editor and the map editor's Encounters
tab edit them. Not built yet: encounter modifier sets (genes, traits),
hidden nodes (a rare-catch spot on a few cells), ambushes, chains, swarms' triggers.

### Encounter leads and timed field effects (F1, D-27 sections 1 and 2)

The tables, methods and conditions above stay as they are; a game layers
its rules on top of them (every rule is manifest data, effect logic or
ruleset):

- **The lead** (`battle::lead_of()`, `include/capsule/battle/encounter_lead.h`):
  the structure's `"lead": {"when": EXPR}` (a growth-style expression over a
  member's record: its roles, stats, species fields) names the first member
  for which it holds; without it, the first member with hit points.
- **Lead rules.** `battle::LeadScope scope(state, party);` attaches them to
  every roll (`encounter::set_rules()`; the scope gives the rolls back when
  it ends). A roll then runs the `encounter` hook (value = the method) on
  the lead in a field context: the game's `global` owners, the lead's
  passive, its held item. Their primitives change the plan, each operand
  interval-checked by the effect compiler: `encounter_rate N D` (the odds x
  N / D), `encounter_levels LO HI` (the entry's bounds moved),
  `encounter_level_bias N`, `encounter_skip_below LEVEL`,
  `encounter_type TYPE N D` / `encounter_field FIELD V N D` (with odds N in
  D the pick keeps to that type's / that species field value's entries);
  `block` stops the roll. Then the ruleset pipelines `encounter_rate`
  (inputs rate_n, rate_m, rate_scale (Q8), lead_level, method; outputs sure,
  chance, out_of) and `encounter_level` (min_level, max_level, lead_level,
  level_bias, method; output level) decide the odds and the level; both are
  optional (the defaults draw exactly what the tables draw on their own) and
  refuse point steps. `battle::last_encounter()` traces each roll (odds,
  lead, pick, why it ended) for debug lines.
- **The creature.** After `field::take_encounter()`,
  `battle::make_encounter(r, wild, rng::gameplay)` makes the record and runs
  the `encounter_made` hook (holder = the lead, target = the creature: e.g.
  `target.role(trait) = holder.role(trait);`; value = its level):
  `encounter_reroll` picks again (at most 3 times), `block` means no
  encounter.
- **Timed field effects** (`include/capsule/script/field_effects.h`): the
  structure's `"field_effects"` [{name, label, unit `steps` | `minutes`,
  stack `refresh` | `add` | `refuse`, expiry message, persists}]. Item
  `use_field` logic starts one (`field_effect start NAME N;`: refused, the run
  fails and the item stays), encounter hooks and other logic read it
  (`field.effect_active(NAME)`, `field.effect_remaining(NAME)`), scripts use
  `field_effect start NAME N [-> var(ok)];`, `field_effect active NAME ->
  var(v);`, `field_effect remaining NAME -> var(v);`. The script compiler
  keeps each in an implicit permanent u16 variable `field_effect_<name>`
  (so the save image's Script section holds it under the save shape and
  remap rules; record the shape: `tools/save_shape.py record scripts.json`)
  and gives an effect with an expiry message the script
  `field_effect_end_<name>`. The overworld runner counts the walker's steps
  and `clock::minutes()` (the game's port, default: never moves) and runs
  the expiry script when an effect ends; a loaded save stops the effects
  that do not persist.
- A ward that keeps weaker creatures away is pure data: an item whose `use_field` starts the
  effect, and a global rule `on encounter { if field.effect_active(ward) {
  encounter_skip_below holder.level; } }` (the Starter's `effects/field.ceffect`).

### Field actions, darkness, flight points and the escape point (F2, D-27 section 3)

Moves used outside battle (crossing water, clearing a bush, lighting a dark
cave, flying, escaping) are data: the engine provides the dispatch and a few
generic mechanisms, every rule and name is manifest data and script.

- **Field actions** (structure.json `"field_actions"`,
  `tools/battlec/field.py`): {name, label, `requires`, `target`, `ask`
  (default true), `prompt` (default "Use LABEL?"), `script`}. `requires` is
  the growth `when` language over a party member (roles, `stat.NAME`,
  `species.NAME`) plus `knows.MOVE` (the move's lower-case identifier),
  `passive` (`passive == passive.NAME`) and the game state `flag.NAME` /
  `var.NAME` (flags and variables of the scripts manifest: a badge, a story
  step); it compiles to one more growth rule each
  (`kFieldRules.first_action_rule` + the action's index) and the battle
  data reads the game state through the script bank's references
  (`rules::field_state()`). The member that uses an action is the first in
  party order its requirement holds for. `target` is `{"behavior": NAME}`
  (a world behavior), `{"object": KIND}` (a world `object_kinds` name a map
  object's `"kind"` gives), `{"tag": TAG}` (a menu action on maps carrying a
  world `map_tags` name) or none (a menu action anywhere). The script
  compiler resolves the targets against the world, the script, the flags and
  variables, gives an asking action the implicit script
  `field_action_ask_<name>` (the prompt as yes / no, then the script) and
  every action the implicit variable `field_action_member` (u8, map: the
  member's party slot).
- **Dispatch** (`capsule/battle/field_actions.h`): a `battle::ActionScope
  scope(party);` attaches the dispatcher to the field
  (`field::set_action_rules()`). Pressing A facing an object of a kind
  without a script of its own, or a cell whose behavior the walker cannot
  enter as it is (water on foot), asks it: the first action on that target
  a member may use runs (`field::Source::Action`; the object is the
  script's `self`, so `hide self;` clears a bush). `battle::menu_actions()`
  lists the menu actions usable on the shown map, `battle::start_action()`
  writes the member and gives the script for `overworld::run_script()`.
  Scripts ask `field_action available NAME -> var(v);` (the member + 1, 0
  for none).
- **Vehicle classes** (exists, wired): a world vehicle class with
  `"dismount": true` that steps onto a cell only class 0 enters becomes
  class 0 (leaving the water); a wade script is `vehicle swim;` and one
  scripted step.
- **Objects** (exist, wired): `hide self;` / `move self DIR;` from the
  action's script.
- **Darkness** (new): a map's `"darkness"` (0 lit to 4) on entering, a
  script's `darkness N;` until the next map. The overworld runner draws a lit
  box around the walker (window 0) and darkens the map layers and sprites
  outside it with the Darken blend; BG0 stays bright when it is the text
  layer. Register writes only (WIN0H / WIN0V / WININ / WINOUT / BLDCNT /
  BLDY each frame of a dark map), the same on every target: the soft video
  model of the desktop, headless and Dreamcast builds has windows and
  blends; the GBA build's code grows by the runner's apply_darkness() and
  the statement (see the package report for the sizes).
- **Flight points** (new): the world's `"flight_points"` [{name, label,
  map, x, y, facing}] each get an implicit permanent flag `flight_<name>`
  (so a save keeps them); scripts `flight record NAME;`, `flight visit NAME
  [transition];`, `flight menu -> var(v);` (the port's paged menu of the
  recorded ones; v = 1 when it flew).
- **The escape point** (new): a map with `"escape_point": true` is
  outdoors; a warp from it to a map without records where the walker stood
  (one step back, facing away) in the implicit permanent variables
  `escape_map` / `escape_x` / `escape_y` / `escape_facing`; scripts `escape
  [transition] [-> var(ok)];` warp back there.
- The Starter's actions (D-27 decision 3, each asking): `wade` (Tide Slam or
  Torrent, on water: swim), `clear` (Vine Lash or Spore Burst, on a bush
  object: it goes), `light` (Mend Light or Prism Beam, a menu action on dark
  maps: `darkness 0;`). The private reference-ruleset repository (not part of this release; see
  [Tools and CI](TOOLS_AND_CI.md#the-reference-ruleset-repository-e11)) has the classic ruleset's set.

### Map events (B-3)

A map's `"events"` hold its warps, triggers and signs; `"area"` names its
area root (B-2: the outdoor map whose area an interior is in). The world
names the scripts manifest they draw scripts, flags and variables from
(`"scripts"`; the game's project.json must name the same file) and the step
dispatcher's hooks (`"step_hooks"`). `tools/event_compiler.py` documents
every key; the map compiler calls it.

```json
"area": "town",
"events": {
  "warps":    [{"at": [4, 9], "to": "house", "warp": 0},
               {"at": [0, 12], "to": "route1", "warp": 0, "trigger": "press", "direction": "left"}],
  "triggers": [{"at": [5, 9], "script": "rival_stops_you", "var": "rival_state", "value": 0}],
  "signs":    [{"at": [3, 3], "script": "town_sign", "facing": "up"},
               {"at": [12, 10], "script": "hidden_potion", "flag": "found_potion"}]
}
```

- Records are bucketed per chunk: the chunk directory names each chunk's
  first event and count, and an event (12 B) holds its chunk-local cell, so
  a step looks at one chunk's events (`map::find_event()`). Warps (8 B)
  hold their landing cell, resolved by the compiler. An event on elevation 0
  matches any level.
- **Field.** Arriving on a cell checks, in the original games' order, a trigger (with a
  variable condition, only while the variable holds its value), a step warp,
  the step hooks that are due, then the encounter roll; pressing A
  checks a sign on the faced cell (its facing rule, its flag clear);
  pressing a press warp's direction while standing on it warps. The first
  that fires stops the walker until the game takes it with
  `field::take_event()`: run its script, or call `field::warp()`.
- `field::enter(map, x, y, facing)` shows a map with the walker (counters
  kept), clears the map bank and, across areas, the area bank
  (`script::change_map()`), and runs the map's load scripts
  (`script::enter_map()`).
- **Step hooks:** every period-th step (a power of two; the compiler spreads
  phases) `field::game::step_hook(id)` runs in game code and may ask for a
  script. `world_ids.h` (generated with the world) names the hooks
  (`STEP_HOOK_...`) and the map indices (`MAP_...`).

Not built yet: weather and other coord-event kinds, secret bases and
counters (interacting across a counter tile).

### Map objects (B-21, pages D-17)

A map's `"objects"` (at most 15: sprite slots 1-15, the walker has slot 0)
are its NPCs, spotters (opponents who walk up to the player) and pickups. `tools/object_compiler.py` documents
every key.

```json
"objects": [
  {"name": "guard", "at": [8, 4], "sprite": "sprite:NPC_GUARD", "facing": "down",
   "script": "guard_talk", "flag": "guard_gone",
   "pages": [{"when": {"flag": "badge_1"}, "movement": "wander", "range": [2, 2], "script": "guard_after"}]},
  {"name": "youngster", "at": [12, 9], "sprite": "sprite:YOUNGSTER", "facing": "left",
   "movement": "look_around", "script": "youngster_battle",
   "spotter": {"sight": 4, "until": "beat_youngster"}}
]
```

- **Records** (ROM): a 32 B template per object (sprite key, cell,
  scripts and flags as 16-bit IDs, movement, range, sight, trigger, speed,
  frequency, options, its route and pages) and 28 B pages.
  `world_ids.h` names local IDs `OBJ_<MAP>_<NAME>`.
- **Spawning:** `field::enter()` spawns the map's objects whose hide flag is
  clear, each with its active page (see Eventing below).
- **Movement** (the classic handheld games' set plus RPG Maker XP's): `static`, `look_around`
  (random turns), `wander` (random steps in its range box), `pace_vertical`
  / `pace_horizontal`, `approach` (steps toward the walker) and `custom` (its
  move route). Random choices use `rng::gameplay`. `schedule` waits for the
  world clock (C-05).
- **Collision:** objects block the walker and each other, on the cell they
  stand on and, while moving, the one they leave (`Blocked::Object`), unless
  they have the `through` option.
- **Talking:** A on a standing object with a script turns it to the walker
  and requests the script (`Source::Object`, `FieldEvent::object`); objects
  come before signs.
- **Spotters (E-8):** while the walker stands, a spotter whose sight is on
  (its until flag, which every spotter needs, clear) that faces it within its sight, with every cell
  between passable and free, spots it, waits 30 frames, walks up, and its
  script is requested (`Source::Spotter`). The first in local-ID order
  wins; there is no "!" emote yet.
- **Locking:** once a script is requested, objects stop moving on their own
  until the script ends (`field::release()`; the overworld runner calls it,
  and `field::lock()` for the scripts it starts itself); scripted walks and
  move routes still run. Scripts start with the object as `self`.
- **Scripts:** `move <obj> <dir> [steps];` (through anything, as the original games'
  scripted movement; waits until it stands), `face <obj> <dir>|player|self;`,
  `hide <obj>;`, `show <obj>;`, with `<obj>` = `self`, `player` or a local
  ID (the list index, as a number or a script constant). The overworld
  runner implements `script::world` (`capsule/script/port.h`).
- **Order on entering a map:** the map's on_transition and on_load scripts
  run first, then the objects spawn, so load scripts change objects
  through their hide and page flags (as the original games do).
- **After a script:** pages are chosen again (so a beaten spotter gets its
  new line at once) and Static objects turn back to their facing. Over a map
  whose layers use BG0 there is no text box.

Not built yet: schedules (C-05 world clock), the "!" emote, several
spotters approaching together (battle formats), the cheap per-chunk spotter
candidate list of E-8 (15 objects are scanned), and objects that are not
16 pixels wide on the cell grid.

### Eventing (D-17)

RPG Maker XP's eventing, on top of the classic handheld games' scripts (design: D-17).

- **Pages:** an object's `"pages"` replace fields of its base record while
  their conditions hold: `when` {`flag` set, `var` >= `value` (or ==
  with `"equal": true`), `self` switch A-D on}, all of which must hold. The
  last page whose conditions hold wins (as RMXP); none means the base
  record. A page sets only the fields it names (sprite, script, movement,
  range, facing, trigger, speed, frequency, options, route) or hides the
  object (`"hidden": true`). Pages are chosen when the object spawns and
  again whenever a flag, variable or self switch changes.
- **Self switches:** four per object (A-D), kept in the world's RAM
  (`World::self_switches`, permanent like flags); only objects whose scripts or
  pages use them (or that ask with `"self_switches": true`) get bits.
  Scripts: `set self(A);`, `clear self(B);`, `if self(A) { ... }`.
- **Triggers:** `action` (A, the default), `player_touch` (the walker
  bumps into it, or steps onto it when it has `through`), `event_touch`
  (it bumps into the walker, or the walker into it), `autorun` (its script
  runs at once, with the player locked, until a page change stops it) and
  `parallel` (its script runs beside the player in a context of its own,
  up to `overworld::kMaxParallel`, restarted when it ends).
- **Move routes** (`tools/route_compiler.py`): an object's `"route"`
  `{"steps": [...], "repeat": true, "skip": false}` with `movement:
  "custom"`, or a script's `route <obj> [repeat] [skip] [nowait] { steps; }`
  and `wait_moves;`. Steps: `up`/`down`/`left`/`right [n]`, `random`,
  `toward`, `away`, `forward`, `backward`, `jump dx dy`, `wait n`, `face
  <dir>`, `turn right|left|around|random`, `set`/`clear FLAG`, `speed n`
  (1-4), `frequency n` (1-5), `walk_anim`/`step_anim`/`direction_fix`/
  `through`/`on_top on|off`, `sprite sprite:NAME`, `show`, `hide`. `skip`
  skips a step that is blocked instead of waiting; a repeating route
  needs a step that takes time. Routes respect collision unless `through`.
- **Speed, frequency and options** per object: speed 1-4 (2 is the
  walker's walking pace), frequency 1-5 (how often it moves on its own),
  `walk_anim`, `step_anim`, `direction_fix`, `through`, `on_top`.
- **Commands:** `input_number N -> var(v);` (N digits in the text box: Left /
  Right pick a digit, Up / Down change it, A enters, B gives 0), `wait_key -> var(v);`, `msg_pos bottom|middle|top;`,
  variable `*=`, `/=` and `%=`, conditions on `pos_x(obj)`, `pos_y(obj)`,
  `facing(obj)`, `map_index`, `steps` and `timer`, `warp MAP x y [dir]
  [transition];`, `place <obj> x y [dir];`, `scroll <dir> N [speed S];` /
  `scroll back;` (pans the camera), `tint r g b strength [frames];` / `tint
  off [frames];`, `flash r g b frames;`, `shake power frames;`, `timer
  start SECONDS;` / `timer stop;` (shown at the top right while no
  message is up; it counts through transitions) and `encounters
  on|off;`. Screen effects leave the text box's colors alone.

Not built yet (D-17): audio commands, pictures and battle animations,
weather (B-22), shops, name input and menu / save calls, panoramas and fog.
Parallel scripts should not show messages (nothing stops them yet).

### Locations, banners and transitions (D-17)

`world.json` names **locations** and each map says which it belongs to
(`"location": "park"`); maps of one location share its name, so walking
between them shows nothing. `tools/location_compiler.py` documents every
key.

```json
"transition": "fade",
"banner_styles": {"box": {"ink": "#f8f8f8", "shadow": "#303030", "fill": "#284878",
                          "border": "#f8f8f8", "frames": 120, "slide": 8},
                  "sign": {"frame": "banners/sign.png", "ink": "#302010", "shadow": "#c8a070",
                           "fill": "#e8c890", "frames": 120, "slide": 8}},
"locations": {"cave": {"name": "Echo Cave", "banner": "box",
                       "preview": {"show": "first", "sequences": [{"sequence": "sequences/cave_preview.json"}]}}}
```

- **Name banners**: entering a map of another
  location slides its name in at the top left in its banner style (a
  bordered box, or a 24x24 frame image cut into 8x8 corners and edges),
  holds it `frames` frames and slides it out. Banners use the text layer
  (BG0, palette bank 13); the game's HUD steps aside while one shows,
  and a message box closes it.
- **Location previews**: a
  location may play a sequence (C-02) full screen between the two halves
  of the transition, `always` or on the `first` visit (visits are kept in
  RAM, `Locations::visited`), each sequence with its own conditions.
- **Warp transitions:** a warp's `"transition"` (or a script's `warp`)
  picks `none`, `fade` (black), `white`, `mosaic`, `wipe_h`, `wipe_v` or
  `box`; without one the world's `"transition"` is used.
- **The overworld runner** (`capsule/overworld/overworld.h`) plays all of
  this: a game's overworld scene calls `overworld::start()` once and
  `overworld::update(intent)` each frame, and takes encounters with
  `field::take_encounter()`.
