# Samples, sandboxes and test apps

The programs that ship with the engine: the samples, the module sandboxes and the test apps.

## Sample projects

The two samples the release ships as downloads, `apps/starter_battle` and `apps/sandbox_map`, are also sample
projects: a user copies one into a folder of their own and opens, edits and builds it like any game. In the
toolkit's Studio that is **Open a sample...** (start window and Studio menu); on its command line,
`capsule-studio sample list` and `capsule-studio sample copy NAME DEST`. The copy builds with the umbrella's
`./build PRESET --game DEST` ([BUILDING.md](BUILDING.md#building-a-game)).

An app is a sample when its folder has a `sample.json` (`capsule.sample/1`) beside `reference/project.json`:

| Key | What it holds |
|---|---|
| `schema` | `capsule.sample/1` |
| `id` | the app folder's name (`capsule-studio sample copy ID`) |
| `name`, `description` | what the Studio lists: a name and one line on what the sample shows |
| `version` | from 1; a copy records it in `editor.sample` of its project manifest |
| `shared` | engine folders the app reads that a copy brings along: `{"from": "../../samples/starter", "to": "data/starter"}`, a battle data template folder (its `template.json` lists the files) and where a copy keeps it |
| `editor` | notes (the toolkit's) |

The engine keeps one copy of each sample, the app itself, which the engine's build, its tests and the release
downloads use as before. A copy is made from it each time:

1. every file of the app folder but `sample.json` goes to the same place in the new folder (the app's
   `README.md` too);
2. the shared folder's template files go to its `to` folder (`data/starter/`: battle.json, structure.json,
   ruleset.json, effects/, assets.json), with `editor.template` recorded as for a new game;
3. each path in the copied JSON manifests that led into the shared folder (the project manifest's `"battle"` and
   `"assets"`, the world's and the scripts' `"battle"`) now leads to its copy;
4. the project manifest gets the new game rules: `"name"` in build form (letters, digits and `_`; the folder's
   name by default), the ROM header's title from the name and the game code `CNEW`.

So that copying keeps working, `tests/test_sample_projects.py` (ctest `sample_project_tests`) checks every
`sample.json` and its app: a path in the app's JSON manifests that leads out of the app folder must lead to a file
a shared template lists; no `#include` of its sources leads out; and its `"libraries"` are ones `capsule_game()`
links for any game. Those are `test_kit` and `battle_kit` (`apps/common`): CMakeLists.txt defines them on every
platform that builds games, outside the engine's own test switches, so a game built with `CAPSULE_GAMES` or in a
parent repository may name them too. The toolkit runs the same check on the finished copy
(`capsule-studio sample check DEST`).

## Starter battle

`capsule_starter_battle` (`apps/starter_battle`) plays the Starter sample like the battle sandbox: the three
rivals (Duo Ash & Bly in 2 vs 2), unowned battles (1 vs 1, 2 vs 2), a 2 vs 2 played by the AI on both sides and
"Heal party". The party menu and the command menu show MP and EP; the move menu shows four of up to eight moves at
a time (Up / Down past a page's edge turns it), each with its cost, type and power, and greys out a move whose cost
cannot be paid (with none payable, FIGHT uses Strike). It links the engine's default generator. SELECT on the
first menu opens the About page (the engine's licence and the devkitPro credit); B, A or SELECT go back.

## Battle sandbox

The battle module's sandbox is `capsule_starter_battle` (above). The classic
sample's sandbox (`sandbox_battle`: unowned creatures and data parties in
1 vs 1, 2 vs 2 and 3 vs 3, presenters with slides, fades and an entrance
sequence) moved with that sample to the reference-ruleset repo (E11), which
builds it against this engine with its Method-1 generator.

## Map sandbox

`capsule_sandbox_map` walks the sandbox world in `apps/sandbox_map/data`
(shapes only): a meadow with grass, flowers, tall grass, trees you walk
behind, an animated pond and river with bridges, ledges, an ice patch with a
rock to stop on, push arrows, a raised plateau with cliffs and stairs
(elevation 4 over ground at 3), an unpainted patch and a left-out chunk
(both show the border), and an L-shaped cave at negative coordinates.

It starts on the **showcase** map, which shows every map feature at once
(built with capsule-studio's core functions, the ones its editor and command
line use):

- Three tile layers whose draw priority is not their BG number: ground on BG1
  at priority 3, geometry (walls, rocks, fences, trunks) on BG3 at priority 2,
  canopy (tree tops, a stone arch, lamp globes, a bridge deck) on BG2 at
  priority 1, so the walker passes under the canopy.
- The attribute layer: water (not passable on foot), ice, ledges, a conveyor,
  tall grass with encounters, environment tags (1 on the grass, 2 on
  paths; see the cell value in the HUD) and walls.
- Two plateaus at elevation 5 with stairs (elevation 0) down to the ground
  at 3, joined by a bridge (15): at level 5 the walker crosses over the deck,
  at level 3 it walks under it along the path (the world gives elevation 5
  sprite priority 1).
- Unpainted cells inside a chunk (the border shows there and blocks), and a
  2x2 border with a pattern on each layer (a tree and animated flowers).
- Tile animations (water, flowers), an image sheet with a transparent color
  (drawn as shapes, `#ff00ff` keyed out) and metatiles mirrored with tile
  flips (the arch halves, the fence end, the deck's lower rail).

- D-pad walk, hold B to run; SELECT shows or hides the HUD (hidden at start;
  map, cell, elevation, behavior, motion, steps, encounter steps, why the last step was blocked).
- L: collision view (red blocked, blue not passable on foot, yellow moves the
  walker, green encounter; the digit is the elevation). R: noclip.
- START opens the menu (SELECT there opens the About page: the engine's licence,
  the devkitPro credit and SDL2; A, B, SELECT or START close it): go to a map, night on / off, the vehicle class (A2:
  class 1, swim, crosses water at 2 pixels a frame and cannot run; F2: only
  water, and it steps back onto land on foot), use a Ward charm, swap the
  lead (F1), the first field action usable here, the flight menu, escape
  (F2) or back to the start.
- Encounters: the showcase's west grass is slot 0, the east grass slot 1
  (night-only creatures), the meadow uses a shared table; species come from
  the battle sandbox's invented data. There is no battle here: the HUD's
  bottom rows show the last encounter (species, level, method, slot) and how
  many there were.
- Encounter leads and timed field effects (F1, D-27): the sandbox carries the
  Starter's battle data, a party (Breezle level 5, whose talent Quick Feet
  raises the encounter rate x3/2 while it leads, and Sproutle) and three Ward
  charms; a `battle::LeadScope` attaches the lead rules. The HUD shows the
  lead (row 2: species, level, talent), the Ward's steps left and the last
  roll's skip level (row 3), and a debug line of each roll (row 16: its odds,
  the creature picked and how it ended: miss, skip, met). The menu's "use
  Ward charm" starts the timed field effect (120 steps; nothing below the
  lead's level comes; "The Ward charm's glow fades." when it ends) and "swap
  the lead" puts Sproutle first. Each encounter's creature is made through
  `battle::make_encounter()` (the lead's encounter_made hooks), then dropped.
- Field actions (F2, D-27 section 3): the party also has Tidepup (Tide Slam)
  and Glimmet (Mend Light). A facing the pond asks "The water is calm. Wade
  across?" (yes: swim, one step onto the water; stepping back onto land
  walks again); A facing the bush at 12,9 asks to clear it (it comes back
  when the map is entered again). The cave is dark (level 3); its sign at
  -6,-5 says whether anyone could light it, and the menu's "field action"
  row runs Light there. Walking into the cave from the showcase records the
  escape point (the menu's "escape" returns there); entering a map records
  its flight point (the menu's "flight menu" lists them). The HUD's blocked
  row shows DARK n on a dark map.
- Events (B-3; scripts in `data/scripts/`): a welcome trigger one step south
  of the start (once), a sign north of it (read facing up with A), a hidden
  berry at 12,10, a step warp at the showcase's west edge (0,9) to the cave
  (in the showcase's area) and a press warp at its east edge (39,9, press
  right) to the meadow, which warps back from 0,0 (press left). The step
  hook "pedometer" counts every 16th step and shows a message every 64.
  Messages and answers use the engine's text box (`script_port_hal.cpp`)
  over the map; the HUD hides while a script runs. The HUD's row 17 shows
  the last event, the berries found and the pedometer.
- Objects (B-21): a guide in the plaza (talking to him sends the gatekeeper
  off the meadow exit, with `move` and `hide`), a stroller wandering east of
  it, a sentry pacing between the hedges, a spotter on the path facing west
  (sight 4: it walks up and talks; afterwards its page gives it another
  line) and the gatekeeper standing on the meadow exit.
- Eventing (D-17): a jogger running laps on a custom move route (talk to it
  to race the timer to the meadow), a box with a self switch (a berry, then
  "empty"), a show-off north of the plaza (shake, flash and a tint), a
  lookout who pans the camera as far east as the number you enter, and an
  invisible puddle at 8,9 (`through`, `player_touch`).
- Locations (D-17): "Showcase Park" (box banner), "Sunny Meadow" (a wooden
  sign banner) and "Echo Cave" (box banner and, on the first visit, a
  preview sequence). The cave warps use a mosaic and a vertical wipe, the
  meadow warp a horizontal wipe; others fade.
- START: menu (pick a map, night on / off, or go back to the start).

## Sequence sandbox

`capsule_sandbox_sequence` plays the five test sequences full-screen through
the engine's real port (`src/seq/sequence_port_hal.cpp`), with placeholder
shape art from `apps/sandbox_sequence/data/assets.json`. Menu: pick a
sequence, toggle its conditions, set the song length (audio is silent; songs
only run for that many frames, so song-timed waits and exits still happen),
A to run. Playing: the pad goes to the sequence; L+R pause, then A steps one
frame, START resumes, B stops. When a sequence ends, the result screen shows
the exit, the stage, the frame count, and which features the port could not
show yet.

What plays today: layers (regular and affine, with each image's declared
priority), sprites with animation, scale and rotation, semi-transparency,
screen fades, alpha blending, windows, palette fades, text pages and the
typed text box. Not yet, and listed on the result screen when used: the
view-only map (C-03), scanline effects (A-7), particle emitters (A-8),
palette animations (A-6/A-9) and audio. The OBJ window is drawn but changes
nothing visible yet, since sequences have no command that sets what it does.

## Test apps

Each engine system has a small app that exercises it alone, so it can be
checked on any target without the rest of a game. They build with every app
(`capsule_test_*`; a `.gba` and its ELF on the GBA, `.elf` / `.cdi` on the Dreamcast) unless
`CAPSULE_TEST_APPS` is `OFF`. L / R change page where an app has pages.

| App | System | What it shows | Controls |
|---|---|---|---|
| `capsule_input_viewer` | input (B-11) | buttons, mapped actions, shoulder filter events | — |
| `capsule_test_video` | `hal::video` | 1 palette banks 4-15; 2 tile and 2×2-block flips; 3 BG1-BG3 overlapping | 3: D-pad scroll, A layer, B priority, START on/off |
| | | 4 the four screen sizes across screen-block seams | 4: D-pad scroll (hold A: fast), B size |
| `capsule_test_vram` | VRAM upload queue (A-5), seeding (A-4) | queue statistics (cycles and bytes of the last VBlank, waiting and peak requests, overruns, early landings), how many frames a streaming job took, the gameplay stream's state; 180 streamed tiles on BG1 | A stream 32 KB, R stream it 8 times, B fade the tiles' bank, START flood the must-land ring |
| `capsule_test_script` | `capsule/script` | the demo scripts (messages, yes / no, menus, loops, call tables, a waiting native, a map trigger), the VM state, flags and variables | idle: Up/Down pick, A run, START enter the map, SELECT clear banks, L new day, R new area; running: the script's own prompts |
| `capsule_test_text` | `capsule/text` | 1 every glyph by code; 2 one line per palette bank; 3 newlines, edge columns, a 32×32 layer that wraps | 3: D-pad scroll |
| `capsule_test_sequence` | `capsule/seq` | picks a test manifest, runs it, shows stage, clocks, fade/blend levels, each track's position and wait, and the last port calls | menu: Up/Down, A run or toggle a condition, Left/Right list length; running: L+R pause; paused: A one frame, START resume, B menu |
| `capsule_sandbox_party` | `capsule/inventory`, `capsule/save` | the Starter's party (pools, status), bag pockets with item use, shops, the save image (sizes, copies, write / read) | L/R page; Bag: Left/Right pocket, A use (member, slot), B back; Shop: A buy; Save: A save, START load, SELECT reset |
| `capsule_test_audio` | `capsule/audio`, `hal::audio` | the audio tour (every sound, then fades / pause / volume / pitch / tempo / mono / a lag frame, then clips): its phase, each player's tracks, pause, sound and clock, the audio step's cycles (GBA: timer 1) | runs by itself; when done: A again, SELECT mono / stereo |
| `capsule_sandbox_audio` | `capsule/audio` | sound number and clip voice, output, reverb, the DMA counter, every player (tracks, pause, sound, clock, fade), the step's cycles | Up/Down sound, A play (B held: pause / resume), B stop, Left/Right clip, START play clip, L fade out, R fade in, SELECT mono |
| `capsule_test_battle` | `capsule/battle` | picks a scenario (1v1 unowned and party, 2v2, 3v3 AI vs AI, statuses, weather), steps the engine one unit at a time; positions with HP, status and stages, the event texts | menu: Up/Down, A start; running: A one step, START run / pause, R next seed, B menu |

The sequence app links its own port instead of `sequence_port_hal.cpp`: it
records every call on screen and simulates what flows wait on (a song lasts
600 frames, typed text keeps the printer busy 30 frames), and it answers the
game hooks from the menu. The sequence compiler emits stage, exit, condition
and list names for it (`<symbol>StageNames` etc.); they are `inline
constexpr`, so they reach a ROM only when something uses them.

New engine systems get an `apps/test_SYSTEM/` (reference/project.json and
src/test_SYSTEM.cpp, using `apps/common/test_kit.h`) and an entry in the
`foreach(app ...)` list in `CMakeLists.txt`.
