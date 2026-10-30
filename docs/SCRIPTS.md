# Scripts and sequences

The script language for game events, and the sequence player for titles, cutscenes and credits.

## Scripts, flags and variables (B-1, B-2, B-4)

Game events are written in the engine's script language (`.cscript`, UTF-8)
and compiled ahead of time by `tools/script_compiler.py` into a bank of
bytecode, flags and variables (`capsule/script/data.h`). An app names its
manifest with the `scripts` project key; the build also writes
`script_ids.h` (`SCRIPT_…`, `FLAG_…`, `VAR_…`, `NATIVE_…`) for its code.

```
flag met_guide;                      // permanent unless a lifetime is given
flag lamp_on : map;                  // permanent | daily | area | map (B-2)
var coins : u8 0..99;                // u8 | u16 | u32, optional bank and range
const PRICE = 5;
native add(a, b) -> value;           // game actions, implemented by the game (P-13)
native beep(times) waits;            // natives that can take several frames

script shop {
    msg "Coins buy lamp oil.";       // or text:"Asset name" (a text asset)
    wait_button;
    while var(coins) >= PRICE { var(coins) -= PRICE; set flag(lamp_on); }
    if flag(lamp_on) { msgbox "The lamp is lit."; } else { msgbox "Not enough coins."; }
    choice ["Red", "Green", "None"] cancel 2 -> var(answer);
    close_msg;
}

map_scripts room {                   // a map of the world, by name
    on_load -> room_load;            // run at once on entering (must not wait)
    on_frame var(coins) == 3 -> coin_event;
}
```

Also: labels and `goto`, `call s;`, `call var(v) of [a, b];` (declared
targets), `return;`, `end;`, `switch` with `case 1, 2 { }` and `default { }`,
`var(v) = random(n);`, `yesno -> var(v);`, `delay n;`,
`role_get(slot, role, element) -> var(v);` and `role_set(slot, role, element,
value);` (a party record's role, engine or custom, by name or ID; element 0
when left out), the bag statements `give_item` / `take_item` /
`count_item`, `open_bag`, `shop "Name" money var(v);` and `time_of_day`
(G2: see ["Inventory, item use and the save image"](BATTLE.md#inventory-item-use-and-the-save-image-g2-d-26-section-2)), and compile-time
`when [not] game(a, b) { }` for build variants (the manifest's `game` and
`games`). The full grammar is in the compiler's docstring.

**Manifest** (`capsule.scripts/1`): `sources`, `game` / `games`,
`stack_depth` (VM call depth, default 20, at most 32), and optionally
`assets` (checks `text:` references), `world` (checks map names) and
`battle` (the game's battle manifest: its record layout gives role names,
custom roles, element counts, widths and the party size for `role_get` /
`role_set` and record placeholders, and `script_ids.h` `ROLE_<NAME>`).

**Checks** (build errors): undefined or duplicate names, labels and
arities; recursion anywhere in the call graph (the cycle is named) and call
chains deeper than `stack_depth`; on_load / on_transition scripts and
trigger conditions that could wait; constants outside a variable's range
(P-18); trigger limits (16 per map, 8 flags / variables each); scripts over
64 KB. Unreachable statements and unused flags or variables are warnings.

**Runtime** (`capsule/script/script.h`):
- Bytecode uses 16-bit script IDs and 16-bit offsets inside each script (no
  pointers; identical scripts are stored once). Constants and variables are
  distinct operand types.
- A `Context` runs a script and its callers. `update()` runs until the
  script waits (message, button, menu, delay, a native returning Wait), ends,
  or has run 1,000 instructions.
- Variables clamp and saturate at their declared range (P-18). Flags are
  numbered bank by bank; `new_day()` clears the daily bank, and
  `change_map(from_area, to_area)` clears the map bank and, when the area
  root differs, the area bank. Storage is emitted with the bank, sized
  exactly; `save_size()` is the sum.
- `enter_map()` runs on_transition then on_load and arms the on_frame
  triggers. `poll_triggers()` re-checks only triggers whose flags or
  variables changed since they were last false. A true trigger fires again
  until its state changes, as the original games' per-frame polling does.
- Presentation goes through `capsule/script/port.h`. The engine's port
  (`src/script/script_port_hal.cpp`) shows messages in the text box, yes / no
  on its last line, and menus in a box that fits them, and reaches party
  records (`script::party::role_get()` / `role_set()`) in the party given to
  `script::party::attach()`. Natives are `script::game::native()`, which the
  game defines.

Map events (B-3) run scripts from the field: see ["Map events"](DATA_FORMATS.md#map-events-b-3)
under Overworld maps. Map objects (B-21) add `move`, `face`, `hide` and `show`
(see ["Map objects"](DATA_FORMATS.md#map-objects-b-21-pages-d-17)); flags and variables used only by map events and
objects are not reported as unused when the manifest names the `world`.
Not built yet: sound commands and imported text tables with placeholders
(C-3).

## Sequences

`include/capsule/seq/` plays title flows, non-overworld cutscenes and credits
from `capsule.sequence/1` manifests compiled by `tools/sequence_compiler.py`.
A sequence is a list of stages; each stage runs up to 8 command tracks in
parallel and leaves through data-defined exits (all tracks done, skip keys,
timeout, song end, key combos, or an explicit goto). Commands cover layers,
sprites (including scale/rotation ramps), screen and palette fades, blending,
windows, text pages and text-box dialogue, a view-only map camera, music
sync, loops over game-supplied lists, and branches on game conditions.
Manifests also support macros and a condition-gated fast-forward.

The player talks to the rest of the engine through `sequence_port.h`.
`src/seq/sequence_port_hal.cpp` draws with the graphics modules (asset
table, backgrounds, sprites, palettes, text layers) and the video HAL's
fades, blending and windows; `capsule/seq/presenter.h` is what a scene calls
around it (reset, per-frame update, what could not be shown). Calls whose
system does not exist yet are recorded and otherwise ignored, so flows still
run with correct timing. Game hooks (conditions, lists, RNG
entropy) default to `src/seq/sequence_game_default.cpp`; a game supplies its
own. Test manifests live in `tests/data/sequences/`.
