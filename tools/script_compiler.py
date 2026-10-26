#!/usr/bin/env python3
"""Compile a game's scripts (capsule.scripts/1 + .cscript sources) into the
script bank (capsule::script::kBank, B-1, B-2, B-4).

    script_compiler.py <scripts.json> <out.cpp> [--ids <script_ids.h>]
                       [--depfile <out.d>] [--debug-names] [--code-page <page.json>]

The manifest names the sources and the build settings:

    {"schema": "capsule.scripts/1", "sources": ["field.cscript", ...],
     "game": "base", "games": ["base", "other"], "stack_depth": 20,
     "assets": "assets.json", "world": "world.json"}

C10: "vm" (optional) changes how the VM runs, for a game whose original script engine
behaves so: {"resume": "next_frame"} (the instruction after a wait runs on the next
frame), {"random": "modulo"} (`random(n)` is a draw modulo n, drawn even for n = 0),
{"vars": "wrap"} (a u8 / u16 variable of its type's whole range overflows as the type
does instead of saturating). The
"session" lifetime keeps a flag or variable in RAM only: never saved, cleared at a new
game (capsule::script::Lifetime::Session).

Every path is relative to the manifest. `assets` (optional) lets the
compiler check `text:` references; `world` (optional) checks map names.
Inline texts are encoded with the game's code page (tools/text_encoder.py;
--code-page, default the engine's ASCII page).

Field placeholders (D-22 decision 6, optional): names the game's field texts
may use, each bound to a name slot, a variable, a term or a party record
field; and terms (fixed words):

    "placeholders": {"player": {"name": 7}, "coins": {"var": "coins"},
                     "rival": {"term": "rival"}, "lead": {"record": "nickname", "slot": 0}},
    "terms": {"rival": "Rowan", "creature": {"one": "Creature", "many": "Creatures"}}

- name: a name slot of that many characters (1-24) the game sets
  (capsule::script::set_name(), e.g. from a naming screen); saved with the
  flags and variables;
- var: a declared variable, printed in decimal;
- term: a term's text (`"form": "many"` for its plural);
- record: a role of a party record (an engine role, tools/battlec/layout.py
  ROLES, e.g. nickname, or with "battle" any role of the game's layout, its
  custom roles too) of party slot `slot` (0 to the party size - 1);
  placeholder_text() prints it through capsule::script::party::role_get():
  a role of 8-bit elements with more than one element as text codes, any
  other in decimal.

Record roles (D-22 decision 2, optional): "battle": "battle.json" names the
game's battle manifest, whose monster_layout gives the record's roles (custom
roles included), widths, counts and party size; without it the engine roles
of the registry are known, at their registry shapes. script_ids.h then names
every role of the layout ROLE_<NAME> (its role ID).

With a "placeholders" key (even an empty one), inline texts are markup:
`{name}` is a placeholder (FD, 0x80 + its declaration index), `{prompt}`
waits for a button (FA), `{page}` waits and starts a new page (FB),
`{@concept}` / `{@concept:many}` expand a term; braces are not characters
then. Without it braces stay plain characters (texts encode as before). The
world's location names follow the same manifest (tools/map_compiler.py).
script_ids.h names each placeholder PLACEHOLDER_<NAME> (its index; I4: a ':' in the name as '_').

The language (`.cscript`, UTF-8) is engine-generic (B-4). Declarations:

    flag name [: permanent|daily|area|map|session];
    var name : u8|u16|u32 [permanent|daily|area|map|session] [lo..hi];
    const NAME = 12;
    native name(arg, ...) [-> value] [waits];
    script name { statements }
    map_scripts map_name { on_transition -> s; on_load -> s; on_frame <cond> -> s; }
    when [not] game(a, b) { declarations }

Statements: `label:`, `goto l;`, `call s;`, `call var(x) of [a, b];`,
`return;`, `end;`, `if <cond> { } else { }`, `while <cond> { }`,
`switch <operand> { case 1, 2 { } default { } }`, `set flag(f);`,
`clear flag(f);`, `var(v) = <operand>;` (also `+=`, `-=`,
`= random(<operand>)`), `msg "text";` or `msg text:key;`, `msgbox ...;`
(message, then wait for a button), `wait_button;`, `close_msg;`,
`yesno -> var(v);`, `choice ["a", text:b] [cancel 1] -> var(v);`,
`delay <operand>;`, `native name(<operands>) [-> var(v)];`,
`give_item(<item>[, <n>]) [-> var(ok)];`, `take_item(<item>[, <n>]) [-> var(ok)];`,
`count_item(<item>) -> var(v);` (the bag, G2: <item> is an item name of
the manifest's `battle` as a string, or an operand holding its id; n
defaults to 1; ok is 1 when all of them moved), `open_bag -> var(v);` (the
bag menu through the script port: the chosen item's id, 0 for none; waits),
`shop "Name" money var(coins);` (a shop list of the battle manifest's
`shops` through the script port, paid from the variable; waits),
`time_of_day -> var(v);` (the game's time of day, capsule/core/clock.h),
`count_boxed -> var(v);`, `deposit_box(<member>) [-> var(ok)];`,
`withdraw_box(<slot>) [-> var(ok)];` and `open_boxes -> var(v);` (the
storage boxes through the script port, G3: the records they hold, a party
member into the first free slot, a storage slot's record to the party's end,
the boxes menu: the chosen box + 1, 0 for none; waits),
`transform(<member>) -> var(v);` (G4: the member's transformation by the
battle's script-trigger rules through the growth port,
capsule/script/growth_port.h; waits; v = 0 nothing, 1 changed, 2 stopped),
`field_effect start NAME <amount> [-> var(ok)];`, `field_effect active NAME
-> var(v);` and `field_effect remaining NAME -> var(v);` (F1, D-27 section 2:
a timed field effect of the battle manifest's structure, `field_effects`,
capsule/script/field_effects.h: started by its stack rule, ok = 1 started,
0 refused; v = 1 while it runs; v = the steps or minutes it has left),
F2 (D-27 section 3, capsule/script/field_actions.h): `darkness <operand>;`
(the shown map's darkness level, 0 lit to 4, until the next map),
`flight record NAME;`, `flight visit NAME [transition];` and `flight menu
-> var(v);` (a flight point of the manifest's world, `flight_points`: record
it (its implicit flag flight_<name>), warp there (waits), or the menu of the
recorded ones: v = 1 when it warped, 0 backed out; waits), `escape
[transition] [-> var(ok)];` (to the escape point the overworld recorded
when the walker last left a map with `escape_point`: ok = 1 when it warped;
waits), `field_action available NAME -> var(v);` (a field action of the
battle manifest's structure, `field_actions`: v = the first party member it
is available to plus 1, 0 for none). The battle manifest's field actions
are resolved here: their targets against the world (behaviors, object_kinds,
map_tags), their scripts, the flags and variables their requirements read;
an asking action gets the implicit script field_action_ask_<name> (its
prompt, yes / no, then its script) and the dispatcher writes the member's
party slot to the implicit variable field_action_member (u8, map).
F3 (D-27 section 4, capsule/script/step_effects.h): the battle manifest's
step effects become implicit permanent u16 variables step_effect_<name>
(the steps counted towards the next run, 0 to every - 1: scripts may read
them) and their records (every, the damage rule, the battle message ids of
their messages, the script a run with an outcome runs); with a script, the
implicit variables step_effect_member (u8, map: the first member, in party
order, with the run's worst outcome) and
step_effect_outcome (u8, map: 0 none, 1 hurt, 2 survived, 3 fainted).
B1 (D-28 section 1, capsule/script/combine_port.h): `combine open NAME -> var(v);`
(a combination method of the battle manifest's structure, `combine_methods`:
the inputs picked from the party through the menu, the result asked
about, combined; waits; v = 0 backed out, 1 combined, 2 refused),
`combine run NAME (<a>, <b>, ...) -> var(v);` (the given party members,
no question; waits), `combine preview NAME (<a>, ...) -> var(v);` (v = the
result species + 1, 0 none, 65535 a random pick), `combine status NAME
(<a>, ...) -> var(v);` (capsule::combine::Status) and `combine fee NAME
(<a>, ...) -> var(v);` (its fee).
B2 (D-28 sections 2 and 3, capsule/script/nursery_port.h): the battle manifest's
nursery and eggs: `nursery pick -> var(v);` (the party menu; waits; v = the
member + 1, 0 backed out), `nursery deposit <member> -> var(v);` (v = 1 when
it went), `nursery withdraw <slot> [pay var(money)] -> var(v);` (its fee taken
from the money variable; v = 1 done, 0 refused, 2 not enough money),
`nursery count|ready|odds|take -> var(v);` (the records held, whether a
result waits, the method's odds over them, the result into the party),
`nursery levels|fee <slot> -> var(v);` and the nickname prompt
`nursery name <member> -> var(v);` (waits; v = 1 when named). With eggs the
implicit variable egg_member (u8, map) holds the party slot of the egg that
hatched, and the structure's eggs.hatch_script names the script run for it.
`role_get(<slot>, <role>[, <element>]) -> var(v);` and
`role_set(<slot>, <role>[, <element>], <value>);` (a party record's role:
slot and element are operands, the role a role name or ID, element 0 when
left out; the record's value clamps to the variable's range, a written value
is truncated to the role's width), the map object
commands (B-21) `move <obj> down|up|left|right [<steps>];` (waits until it
stands; C8: `... [<steps>] checked [-> var(ok)];` walks only when the
field's movement rules allow every step: terrain, elevation, the walker's
vehicle class, ledges, objects; else nothing moves, ok = 0),
`face <obj> down|up|left|right|player|self|opposite_player;` (C10:
opposite_player turns it opposite the walker's facing, not towards it),
`hide <obj>;`, `show <obj>;`
with <obj> = `self` (the object whose talk or sight started the script),
`player`, C10 `selected` (self, or the walker when no object started the
script) or an operand (a local ID: the map's object list
index, as a number or a constant), `vehicle NAME;` or `vehicle N;` (the
walker's vehicle class, A2: a name from the world's vehicle_classes, which
needs the manifest's `world`, or a class number), DS1 (D-30 section 2)
`override apply NAME;` / `override clear NAME;` (a cell override set of a
map of the manifest's world, the maps' `override_sets`: its cells show their
override until cleared; a persisting set's flag or variable is written, so it
shows again when its map is entered), and
compile-time `when [not] game(...) { }`.

Operands: numbers, constants, `yes` / `no` / `true` / `false`, `var(v)` and
asset references (`item:potion`, their Asset Key v1). Conditions:
`flag(f)`, `<operand> == != < <= > >= <operand>`, `!`, `&&`, `||`, ( ).

Checks (build errors): undefined or duplicate names, labels and arities;
recursion anywhere in the call graph (the cycle is named) and a call depth
beyond stack_depth (B-1); dynamic calls name their targets; on_transition /
on_load scripts and trigger conditions never wait; constants within the
variable's range (P-18); at most 16 on-frame triggers per map reading at
most 8 flags / variables each; scripts under 64 KB. Warnings: unreachable
statements, unused flags and variables.
"""
from __future__ import annotations

import json
import re
import struct
import sys
from collections.abc import Collection, Iterable, Mapping, Sequence
from os import PathLike
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from asset_compiler import CODE_END, encode_text, ManifestError as AssetError  # noqa: E402
from asset_compiler import CHANNEL_MASK as ASSET_CHANNEL_MASK  # noqa: E402
from sequence_compiler import asset_key, ManifestError as KeyError_  # noqa: E402
from sequence_compiler import HEX_BASE, INT16_MAX, INT16_MIN, U16_MASK, U32_MASK  # noqa: E402
import route_compiler as rc  # noqa: E402
import save_shape  # noqa: E402
import text_encoder  # noqa: E402

## Opcode numbers; order must match capsule::script::Op.
OPS = ["End", "Return", "Goto", "Call", "CallTable", "IfFlagClear", "IfFlagSet", "IfCompare",
       "SetFlag", "ClearFlag", "SetVar", "AddVar", "SubVar", "Random", "Message", "WaitButton",
       "CloseMessage", "YesNo", "Choice", "Delay", "Native", "TriggerTrue", "ObjectMove", "ObjectFace",
       "ObjectShow", "ObjectHide", "Route", "WaitMoves", "SelfSwitch", "IfSelfSet", "IfSelfClear", "VarOp",
       "InputNumber", "WaitKey", "MessagePos", "Warp", "Place", "Scroll", "Tint", "Flash", "Shake", "TimerStart",
       "TimerStop", "Encounters", "Vehicle", "RoleGet", "RoleSet",
       # G2 (D-26 section 2): the bag, shops and the game's time of day
       "ItemGive", "ItemTake", "ItemCount", "BagOpen", "Shop", "TimeOfDay",
       # G3 (D-26 section 3): the storage boxes
       "BoxCount", "BoxDeposit", "BoxWithdraw", "BoxesOpen",
       # G4 (D-26 section 4): a party member's transformation
       "Transform", "FieldEffect",
       # F2 (D-27 section 3): darkness, flight points, the escape point, field actions
       "Darkness", "Flight", "Escape", "FieldAction",
       # C8: a walk by the movement rules (`move ... checked`)
       "ObjectMoveChecked",
       # B1 (D-28 section 1): a combination method
       "Combine",
       # B2 (D-28 sections 2 and 3): the nursery and the nickname prompt
       "Nursery",
       # I4 (D-29, imported event scripts): a jump to another script, object locks, message modes, object waits,
       # removals, visibility and template changes, doors, weather, screen fades, metatiles, the respawn point,
       # remembered warps, sound, marts
       "Jump", "Lock", "Release", "MessageEx", "WaitMessage", "WaitObject", "ObjectRemove", "ObjectVisible",
       "ObjectTemplate", "Door", "Weather", "Fade", "SetMetatile", "Respawn", "SetWarp", "Sound", "WaitSound",
       "Mart", "CallLoose", "WaitState",
       # DS1 (D-30 section 2): cell override sets
       "Override"]
## Opcode name -> number.
OP = {name: i for i, name in enumerate(OPS)}
## Comparison operators in capsule::script::Compare order.
COMPARES = ["==", "!=", "<", "<=", ">", ">="]
## Negation of each comparison.
NEGATE = {"==": "!=", "!=": "==", "<": ">=", "<=": ">", ">": "<=", ">=": "<"}
## Lifetime names in capsule::script::Lifetime order (C10: "session" last: kept in RAM only, never saved).
LIFETIMES = ["permanent", "daily", "area", "map", "session"]
## The lifetime cleared on every map change (the implicit map-lifetime variables use it).
MAP_LIFETIME = "map"
## C10: the lifetime kept in RAM only (last; every lifetime before it is saved).
SESSION_LIFETIME = "session"
## Variable types -> (width, lowest, highest).
TYPES = {"u8": (1, 0, 255), "u16": (2, 0, 65535), "u32": (4, 0, 2**31 - 1)}
## Trigger kinds in capsule::script::TriggerKind order (I4: on_resume when the field resumes on the map, on_warp_into
## after a warp's arrival (the first entry whose condition holds), on_dive when a dive / emerge has no connection,
## on_return when the field comes back from a menu or a battle).
TRIGGER_KINDS = ["on_transition", "on_load", "on_frame", "on_resume", "on_warp_into", "on_dive", "on_return"]
## I4: trigger kinds whose entries have a condition (on_frame, on_warp_into).
CONDITION_KINDS = ("on_frame", "on_warp_into")
## I4: trigger kinds whose scripts must not wait (every kind but on_frame: they run to their end at once).
IMMEDIATE_KINDS = ("on_transition", "on_load", "on_resume", "on_warp_into", "on_dive", "on_return")
## Words that cannot name anything.
KEYWORDS = {"flag", "var", "const", "native", "script", "map_scripts", "when", "game", "not", "goto",
            "call", "of", "return", "end", "if", "else", "while", "switch", "case", "default", "set",
            "clear", "random", "msg", "msgbox", "wait_button", "close_msg", "yesno", "choice", "cancel",
            "delay", "yes", "no", "true", "false", "waits", "value", "move", "face", "hide", "show", "self",
            "player", "route", "wait_moves", "input_number", "wait_key", "msg_pos", "warp", "place", "scroll",
            "tint", "flash", "shake", "timer", "encounters", "pos_x", "pos_y", "facing", "map_index",
            "steps", "vehicle", "role_get", "role_set"} | set(LIFETIMES) | set(TYPES)
## Limits shared with capsule/script/data.h.
MAX_STACK = 32
## Arguments per native.
MAX_NATIVE_ARGS = 8
## Options per choice.
MAX_CHOICES = 8
## On-frame triggers per map.
MAX_TRIGGERS = 16
## Flags / variables per trigger condition.
MAX_TRIGGER_DEPS = 8
## Operand tags (capsule/script/data.h).
OPND_CONST8, OPND_CONST16, OPND_CONST32, OPND_VAR = 0, 1, 2, 3
## Text reference tags.
TEXT_ASSET, TEXT_INLINE = 0, 1
## Native without a result variable.
NO_VAR = 0xFFFF
## Choice without a cancel index.
NO_CANCEL = 0xFF
## Bank::deps bit for a variable.
DEP_VAR = 0x8000
## Opcodes that make a script wait.
WAITING_STATEMENTS = {"msg", "msgbox", "wait_button", "yesno", "choice", "delay", "move", "route", "wait_moves",
                      "input_number", "wait_key", "warp", "scroll", "open_bag", "shop", "open_boxes", "transform"}
## The bag statements (G2): name -> {"op": opcode, "counted": takes a count, "result": needs a result variable}.
ITEM_STATEMENTS = {"give_item": {"op": "ItemGive", "counted": True, "result": False},
                   "take_item": {"op": "ItemTake", "counted": True, "result": False},
                   "count_item": {"op": "ItemCount", "counted": False, "result": True}}
## The box statements that move a record (G3): name -> opcode (the operand: a party member / a storage slot).
BOX_STATEMENTS = {"deposit_box": "BoxDeposit", "withdraw_box": "BoxWithdraw"}
## The box statements that only give a value (G3): name -> opcode (`name -> var(v);`).
BOX_QUERIES = {"count_boxed": "BoxCount", "open_boxes": "BoxesOpen"}
## F1: `field_effect KIND NAME ...` kinds in capsule::script::FieldEffectOp order (start takes an amount; active and
## remaining need -> var(v)).
FIELD_EFFECT_KINDS = ["start", "active", "remaining"]
## The statement word of the timed field effects.
FIELD_EFFECT_WORD = "field_effect"
## F2: `flight KIND ...` kinds in capsule::script::FlightOp order.
FLIGHT_KINDS = ["record", "visit", "menu"]
## The statement word of the flight points.
FLIGHT_WORD = "flight"
## The statement word of the darkness level.
DARKNESS_WORD = "darkness"
## C8: the word after a move's steps that makes it a walk by the field's movement rules (`move OBJ DIR [N] checked
## [-> var(ok)];`: every step allowed, or nothing moves; ok = 1 when it walked).
CHECKED_WORD = "checked"
## The statement word of the escape point.
ESCAPE_WORD = "escape"
## F2: `field_action KIND NAME -> var(v);` kinds in capsule::script::FieldActionOp order.
FIELD_ACTION_KINDS = ["available"]
## The statement word of the field actions.
FIELD_ACTION_WORD = "field_action"
## B1: `combine KIND NAME ...` kinds in capsule::script::CombineOp order (open: the menu flow; run: given members;
## preview / status / fee: queries).
COMBINE_KINDS = ["open", "run", "preview", "status", "fee"]
## B1: the combine kinds that wait.
COMBINE_WAITING = ("open", "run")
## B1: the statement word of the combination methods.
COMBINE_WORD = "combine"
## B1: most party members a combine statement names (capsule::script::kMaxCombineMembers).
MAX_COMBINE_MEMBERS = 8
## B2: `nursery KIND ...` kinds in capsule::script::NurseryOp order.
NURSERY_KINDS = ["pick", "deposit", "withdraw", "count", "levels", "fee", "ready", "odds", "take", "name"]
## B2: the nursery kinds with an operand (a party member or a nursery slot).
NURSERY_OPERAND = ("deposit", "withdraw", "levels", "fee", "name")
## B2: the nursery kinds that wait (the party menu, the nickname prompt).
NURSERY_WAITING = ("pick", "name")
## B2: the kinds a game without a nursery may use (the nickname prompt).
NURSERY_ANYWHERE = ("name",)
## B2: the statement word of the nursery.
NURSERY_WORD = "nursery"
## B2: the word before a withdraw's money variable.
PAY_WORD = "pay"
## Tokens ahead of `darkness` where a name operand's ';' stands (`darkness NAME;`).
OPERAND_END_AHEAD = 2
## Most darkness levels of a world (capsule::map::kMaxDarkness; C8: the world's darkness_levels say how many).
MAX_DARKNESS = 15
## C8: the implicit map-lifetime variable that keeps a script-set darkness across a save (level + 1; 0: the map's
## own level; capsule::script::Bank::darkness_var). Declared for a world with a dark map or scripts using
## `darkness`; the map lifetime clears it when the walker goes to another map.
DARKNESS_VAR = "map_darkness"
## Prefix of a flight point's implicit flag (recorded = visited).
FLIGHT_FLAG_PREFIX = "flight_"
## The escape point's implicit variables (capsule::script::EscapeVars order): the map's asset key's high and low
## 16 bits (C8: a key, so a save keeps naming the same map), column and row (+ ESCAPE_BIAS), facing.
ESCAPE_VARS = ["escape_map_hi", "escape_map_lo", "escape_x", "escape_y", "escape_facing"]
## FieldActionInfo::flags bit: the action asks first (capsule::script::kActionAsks).
ACTION_ASKS = 1
## A script ID meaning none (capsule::script::kNoScriptId).
NO_SCRIPT_ID = 0xFFFF
## Most items one give / take moves (BagSlot counts are 16 bits).
MAX_ITEM_COUNT = 0xFFFF
## Operand tag of a game query (capsule/script/data.h kOpQuery).
OPND_QUERY = 4
## Queries in capsule::script::Query order: name -> takes an object.
QUERIES = {"pos_x": True, "pos_y": True, "facing": True, "map_index": False, "steps": False, "timer": False}
## VarOp sub-operations in capsule::script::VarOpKind order.
VAR_OPS = ["*=", "/=", "%="]
## Warp transitions in capsule::script::Transition order ("default" = the warp's own or the world's; I4: silent (a fade
## without the warp's sound), door (walking into a door), fall (through a hole), teleport, spin).
TRANSITIONS = ["none", "fade", "white", "mosaic", "wipe_h", "wipe_v", "box", "silent", "door", "fall", "teleport",
               "spin"]
## Transition byte for "the default".
TRANSITION_DEFAULT = 0xFF
## Text box positions in order.
MSG_POSITIONS = ["bottom", "middle", "top"]
## Self switch letters.
SELF_LETTERS = "ABCD"
## Route operand flag (high bit of the Route flags byte): the script goes on at once.
ROUTE_NOWAIT = 0x80
## Directions in field::Direction order.
DIRECTIONS = ["down", "up", "left", "right"]
## Object operands (capsule/script/data.h): the context's own object, the walker.
OBJECT_SELF, OBJECT_PLAYER = 254, 255
## Highest local ID of a map object (I4: map::kMaxObjectTemplates - 1: a map with object_view lists up to 64
## templates; plus the manifest's object_base).
MAX_OBJECT = 63
## ObjectFace targets beyond the directions: towards the walker, towards self; C10: opposite the walker's facing
## (capsule::script::kFaceOppositePlayer).
FACE_PLAYER, FACE_SELF, FACE_OPPOSITE_PLAYER = 4, 5, 6
## C10: the face words beyond the directions and their ObjectFace targets.
FACE_WORDS = {"player": FACE_PLAYER, "self": FACE_SELF, "opposite_player": FACE_OPPOSITE_PLAYER}
## Warp / Place facing byte: keep the current facing (kKeepFacing).
KEEP_FACING = 4
## Scroll direction byte: back to the player.
SCROLL_BACK = 4

# --- I4 (D-29): the statements of imported event scripts ---
## Object operands beyond the local IDs: the camera (capsule::script::kObjectCamera) and the object a script last
## gave a route (kObjectLast; `wait_moves last;` only).
OBJECT_CAMERA, OBJECT_LAST = 252, 251
## C10: object operand: the selected object (the script's own object, or the walker when it has none;
## capsule::script::kObjectSelected).
OBJECT_SELECTED = 250
## Object words of object_ref() besides self and player (not keywords: scripts may still name things so).
OBJECT_WORDS = {"camera": OBJECT_CAMERA, "last": OBJECT_LAST, "selected": OBJECT_SELECTED}
## C10: the scripts manifest's "vm" object: key -> {value: capsule::script::kVm... bit} (the first value of each key
## is the engine's own behaviour, bit 0).
VM_OPTIONS = {"resume": {"same_frame": 0, "next_frame": 1},
              "random": {"scaled": 0, "modulo": 2},
              "vars": {"saturate": 0, "wrap": 4}}
## Most an object_base may be (local IDs then count from it; capsule::script::Bank::object_base).
MAX_OBJECT_BASE = 1
## `msg` modifiers -> MessageEx flag bits (capsule::script::kMessage...): no wait for the typing, the text at once,
## text that scrolls on by itself.
MSG_FLAGS = {"nowait": 1, "instant": 2, "autoscroll": 4}
## `msgbox` kinds (the compiler writes them out as ops): npc (lock, face the player, message, button, release),
## sign (lock all, message, button, release all), yesno (message, then yes / no into a variable), autoclose
## (message, button, release).
MSGBOX_KINDS = ("npc", "sign", "yesno", "autoclose")
## Lock / Release kind bytes: the context's object (and its own freeze), every object.
LOCK_SELF, LOCK_ALL = 0, 1
## `set_template` kinds in capsule::script::TemplateOp order: its cell, its movement, its cell from where it stands.
TEMPLATE_KINDS = ["pos", "movement", "here"]
## `door` kinds in capsule::script::DoorOp order (wait takes no cell).
DOOR_KINDS = ["open", "close", "set_open", "set_closed", "wait"]
## `weather` kinds in capsule::script::WeatherOp order (set takes the weather).
WEATHER_KINDS = ["set", "reset", "apply"]
## `fade` kinds in capsule::script::FadeOp order.
FADE_KINDS = ["to_black", "from_black", "to_white", "from_white"]
## Fade flags byte: the fade keeps the palettes the scene changes meanwhile (swapped palette buffers).
FADE_SWAP = 1
## Fade speed byte when the statement gives none.
DEFAULT_FADE_SPEED = 0
## `set_warp` kinds in capsule::script::WarpSlot order: the next warp, the dynamic warp, the dive / emerge, the hole,
## the escape warp.
SET_WARP_KINDS = ["next", "dynamic", "dive", "hole", "escape"]
## `sound` kinds in capsule::script::SoundOp order -> the operands they take.
SOUND_KINDS = {"bgm": 2, "bgm_save": 1, "fade_default": 0, "fade_new": 1, "fade_out": 1, "fade_in": 1, "se": 1,
               "fanfare": 1, "cry": 2}
## `wait_sound` kinds in capsule::script::SoundWait order.
SOUND_WAITS = ["se", "fanfare", "cry", "bgm"]
## Most items of one mart (16-bit count).
MAX_MART_ITEMS = 0xFFFF
## Most game route steps (8-bit ids).
MAX_ROUTE_STEPS = 256
## Declaration words that are no keywords (a script may still name a label or a variable so): a named move route, a
## mart list, a game route step.
MOVEMENT_WORD, MART_WORD, STEP_WORD = "movement", "mart", "step"
## Tokens ahead of a declaration word where its '{' (or a step's ';') stands (`movement NAME {`).
DECLARATION_AHEAD = 2
## The compile-time switch word of a glitch variant (`when [not] glitch("id")`).
GLITCH_WORD = "glitch"
## The word after a call's script that makes it loose (`call NAME loose;`: CallLoose, a call that may recurse; when
## the VM's stack is full it goes on as a jump, as an imported game's own VM does).
LOOSE_WORD = "loose"
## The warp word that does not wait (`warp ... nowait;`: the warp starts, the script goes on).
WARP_NOWAIT_WORD = "nowait"
## Warp facing byte bit: the script goes on at once (capsule::script::kWarpNoWait).
WARP_NOWAIT = 0x80
## Statement words of I4 that are no keywords.
I4_WORDS = ("lock", "release", "wait_msg", "remove", "visible", "set_template", "door", "weather", "fade",
            "set_metatile", "respawn", "set_warp", "sound", "wait_sound", "mart", "wait_state", "override")
## DS1: the override statement's kinds (`override apply NAME;` / `override clear NAME;`) and their apply byte.
OVERRIDE_KINDS = ("clear", "apply")

## Stack depth when the manifest gives none.
DEFAULT_STACK_DEPTH = 20
## Smallest value of a signed 32-bit constant.
INT32_MIN = -2**31
## Bound (exclusive) of a signed 32-bit constant; unsigned values from here up are negative as int32.
INT32_LIMIT = 2**31
## Values of 32 bits (2^32), for reinterpreting an unsigned key as signed.
U32_RANGE = 2**32
## Largest unsigned 8-bit value (also the largest 1-byte constant operand).
U8_MAX = 255
## Mask to 8 bits.
U8_MASK = 0xFF
## Length of a "0x" prefix.
HEX_PREFIX_LENGTH = 2
## Base of decimal numbers.
DECIMAL_BASE = 10
## Bits in a byte.
BITS_PER_BYTE = 8

## Most targets of a call table (8-bit count).
MAX_CALL_TABLE = 255
## Largest bound of random(n) (results 0-65535).
MAX_RANDOM_BOUND = 65536
## Most steps of one move (16-bit operand).
MAX_MOVE_STEPS = 65535
## Most digits of input_number.
MAX_INPUT_DIGITS = 9
## Highest key code wait_key stores (1-10, one per GBA button).
MAX_KEY_CODE = 10
## Fastest camera scroll speed (1-4).
MAX_SCROLL_SPEED = 4
## Vehicle classes a world may have (capsule::map::kMaxVehicleClasses); the limit of `vehicle N;` without a world.
MAX_VEHICLE_CLASSES = 8
## Camera scroll speed when the statement gives none.
DEFAULT_SCROLL_SPEED = 2
## Color channels of a tint or flash (red, green, blue).
RGB_CHANNELS = 3
## Largest 5-bit color channel of a tint or flash.
MAX_COLOR_CHANNEL = 31
## Mask of a 5-bit color channel.
CHANNEL_MASK = ASSET_CHANNEL_MASK
## Largest tint strength (16/16 is the full tint color).
MAX_TINT_STRENGTH = 16
## Longest tint, flash or shake, in frames (16-bit operand).
MAX_EFFECT_FRAMES = 65535
## Strongest shake, in pixels.
MAX_SHAKE_POWER = 8
## Longest timer (99:59), in seconds.
MAX_TIMER_SECONDS = 5999

## Largest script, in bytes (16-bit lengths).
MAX_SCRIPT_BYTES = 0xFFFF
## Trigger condition ID meaning "no condition".
NO_CONDITION = 0xFFFF
## Most entries of one map's map_scripts (8-bit count).
MAX_MAP_ENTRIES = 255
## Most entries of a bank table indexed by 16 bits (scripts, triggers, dependencies).
MAX_BANK_ENTRIES = 0xFFFF
## Largest flag or variable bank, in bytes (16-bit sizes).
MAX_BANK_BYTES = 0xFFFF
## Scripts start at even offsets of the code blob.
CODE_ALIGNMENT = 2

## Call graph visit state: on the current path.
VISITING = 1
## Call graph visit state: finished.
VISITED = 2

## Generated array elements per line, by default.
DEFAULT_PER_LINE = 12
## Generated array elements per line for bytes.
BYTES_PER_LINE = 16
## Generated ScriptInfo records per line.
SCRIPTS_PER_LINE = 4
## Generated records per line for the wider records (variables, maps, triggers).
RECORDS_PER_LINE = 2

## Words of a route step that sets or clears a flag ("set name" / "clear name").
ROUTE_FLAG_WORDS = 2
## Fewest positional command-line arguments (manifest and output).
MIN_ARGUMENTS = 2
## Exit status for a bad command line.
EXIT_USAGE = 2

## A manifest object (parsed JSON).
JsonDict = dict[str, Any]

## Field placeholder binding kinds, in capsule::script::PlaceholderKind order (each the key naming it).
PLACEHOLDER_KINDS = ["name", "var", "term", "record"]
## Keys a binding may have besides its kind's.
PLACEHOLDER_EXTRA_KEYS = {"name": set(), "var": set(), "term": {"form"}, "record": {"slot"}}
## Party slots a record placeholder or role statement may name without "battle" (capsule::monster's default
## party size).
PARTY_SIZE = 6
## Role IDs a role statement may name by number without "battle": custom role IDs from CUSTOM_ROLE_FIRST, at most
## MAX_CUSTOM_ROLES (capsule::monster::kFirstCustomRole, kMaxCustomRoles).
CUSTOM_ROLE_FIRST = 0x80
## Most custom roles (capsule::monster::kMaxCustomRoles).
MAX_CUSTOM_ROLES = 32
## Most operands after the role of role_get (element) and role_set (element, value).
ROLE_GET_TAIL, ROLE_SET_TAIL = 1, 2
## Highest element index of a role statement (an 8-bit operand in the VM).
MAX_ROLE_ELEMENT = 255
## Highest value a variable holds (role values above it clamp).
MAX_VAR_VALUE = 2**31 - 1
## Digits of the largest variable value (2**31 - 1): the most a variable placeholder prints.
MAX_VAR_DIGITS = 10
## Bytes before a name slot's codes in Bank::names (its length).
NAME_LENGTH_BYTES = 1
## Element width of a role whose elements are text codes (nickname, owner_name).
TEXT_ROLE_WIDTH = 8
## Term forms (text_encoder.TERM_FORMS): the one form, then the many form.
TERM_ONE, TERM_MANY = text_encoder.TERM_FORMS


## A record role a script names: (role ID, element width, element count).
RoleShape = tuple[int, int, int]


def vm_flags(vm: Any) -> int:
   """!C10: the scripts manifest's "vm" object as capsule::script::Bank::vm_flags (VM_OPTIONS: "resume":
   "same_frame" | "next_frame", "random": "scaled" | "modulo", "vars": "saturate" | "wrap"; a key left out keeps the
   engine's own behaviour).
   @param vm  the manifest's value
   @return the bits
   @throws ScriptError for an unknown key or value
   """
   if not isinstance(vm, dict):
      raise ScriptError("vm must be an object (" + ", ".join(VM_OPTIONS) + ")")
   bits = 0
   for key, value in vm.items():
      if key not in VM_OPTIONS:
         raise ScriptError(f"unknown vm key {key!r} (" + ", ".join(VM_OPTIONS) + ")")
      if value not in VM_OPTIONS[key]:
         raise ScriptError(f"vm {key} must be " + " or ".join(VM_OPTIONS[key]))
      bits |= VM_OPTIONS[key][value]
   return bits


def load_record_layout(path: str | PathLike[str], deps: list[str] | None = None) -> dict[str, Any]:
   """!The record layout of a battle manifest (the monster_layout and stat count of the structure manifest it
   names, E5s; tools/battlec/layout.py), checked against the most generous platform limits (the battle compile
   checks the target's).
   @param path  the battle manifest
   @param deps  list collecting the files read (the battle and structure manifests), or None
   @return compile_layout() result
   @throws ScriptError when they cannot be read or the layout is invalid
   """
   import build_settings
   from battlec.common import ManifestError
   from battlec.layout import DEFAULT_STAT_COUNT, compile_layout
   from battlec.manifest import read_battle
   path = Path(path)
   read: list[Path] = []
   try:
      b, st_path = read_battle(path, None, read)
   except ManifestError as e:
      raise ScriptError(f"cannot read battle manifest {path}: {e}") from None
   if deps is not None:
      deps += [str(p) for p in read]
   stats = b.get("stats")
   limits = build_settings.platform_limits("default")
   try:
      return compile_layout(b.get("monster_layout"), f"{st_path.name}: monster_layout", limits["max_record_bytes"],
                            len(stats) if isinstance(stats, list) else DEFAULT_STAT_COUNT, limits["max_party_size"])
   except ManifestError as e:
      raise ScriptError(str(e)) from None


def load_inventory_names(path: str | PathLike[str]) -> tuple[list[str], list[str]]:
   """!The item and shop names of a battle manifest (G2: give_item / take_item / count_item and shop).
   @param path  the battle manifest
   @return (item names in id order from 1, shop names in index order)
   @throws ScriptError when it cannot be read
   """
   from battlec.common import ManifestError
   from battlec.manifest import read_battle
   try:
      b, _st = read_battle(Path(path), None, [])
   except ManifestError as e:
      raise ScriptError(f"cannot read battle manifest {path}: {e}") from None
   items = [i.get("name") for i in b.get("items", []) if isinstance(i, dict)]
   shops = [sh.get("name") for sh in b.get("shops", []) if isinstance(sh, dict)]
   return items, shops


def field_var_name(name: str) -> str:
   """!The implicit variable of a timed field effect (F1; battlec/field.py var_name()).
   @param name  the effect's name
   @return the variable's name
   """
   from battlec.field import var_name
   return var_name(name)


def load_field_effects(path: str | PathLike[str]) -> list[dict[str, Any]]:
   """!The timed field effects of a battle manifest's structure (F1; tools/battlec/field.py compile_field_effects()).
   @param path  the battle manifest
   @return the compiled records (structure order)
   @throws ScriptError when it cannot be read or a record is invalid
   """
   from battlec.common import ManifestError
   from battlec.field import FIELD_EFFECTS_KEY, compile_field_effects
   from battlec.manifest import read_battle
   try:
      b, st_path = read_battle(Path(path), None, [])
      return compile_field_effects(b.get(FIELD_EFFECTS_KEY), st_path.name)
   except ManifestError as e:
      raise ScriptError(f"cannot read the field effects of {path}: {e}") from None


def load_step_effects(path: str | PathLike[str]) -> list[dict[str, Any]]:
   """!The step effects of a battle manifest's structure (F3; tools/battlec/field.py compile_step_effects(), their
   messages resolved against the battle's messages).
   @param path  the battle manifest
   @return the compiled records (structure order)
   @throws ScriptError when it cannot be read or a record is invalid
   """
   from battlec.common import ManifestError
   from battlec.field import STEP_EFFECTS_KEY, compile_step_effects
   from battlec.manifest import read_battle
   from battlec.texts import declared_messages
   try:
      b, st_path = read_battle(Path(path), None, [])
      return compile_step_effects(b.get(STEP_EFFECTS_KEY), declared_messages(b.get("messages", {})), st_path.name)
   except ManifestError as e:
      raise ScriptError(f"cannot read the step effects of {path}: {e}") from None


def load_eggs(path: str | PathLike[str]) -> dict[str, Any]:
   """!Whether a battle manifest's structure has eggs and a nursery, and the eggs' hatch script (B2; the battle
   compiler checks the rest).
   @param path  the battle manifest
   @return {"eggs": bool, "nursery": bool, "hatch_script": name or None}
   @throws ScriptError when it cannot be read
   """
   from battlec.common import ManifestError
   from battlec.manifest import read_battle
   from battlec.nursery import EGGS_KEY, NURSERY_KEY
   try:
      b, _st_path = read_battle(Path(path), None, [])
   except ManifestError as e:
      raise ScriptError(f"cannot read the eggs of {path}: {e}") from None
   eggs = b.get(EGGS_KEY)
   script = eggs.get("hatch_script") if isinstance(eggs, dict) else None
   return {"eggs": isinstance(eggs, dict), "nursery": isinstance(b.get(NURSERY_KEY), dict),
           "hatch_script": script if isinstance(script, str) else None}


def egg_source() -> str:
   """!The implicit declaration of the eggs (B2): the party slot of the egg that hatched.
   @return the source
   """
   from battlec.nursery import EGG_MEMBER_VAR
   return f"var {EGG_MEMBER_VAR} : u8 map;\n"


def step_effect_source(effects: Sequence[Mapping[str, Any]]) -> str:
   """!The implicit declarations of the step effects (F3), as `.cscript` source: per effect a permanent u16
   variable 0 to every - 1 (the steps counted towards its next run); with any effect naming a script, the member
   and outcome variables that script reads.
   @param effects  load_step_effects() result
   @return the source
   """
   from battlec.field import MEMBER_STEP_VAR, OUTCOME_STEP_VAR, STEP_OUTCOMES, step_var_name
   lines = [f"var {step_var_name(e['name'])} : u16 {LIFETIMES[0]} 0..{e['every'] - 1};" for e in effects]
   if any(e["script"] is not None for e in effects):
      lines.append(f"var {MEMBER_STEP_VAR} : u8 map;")
      lines.append(f"var {OUTCOME_STEP_VAR} : u8 map 0..{len(STEP_OUTCOMES) - 1};")
   return "\n".join(lines) + "\n"


def load_field_actions(path: str | PathLike[str]) -> list[dict[str, Any]]:
   """!The field actions of a battle manifest's structure (F2; tools/battlec/field.py compile_field_action_records(),
   without their requirements' rules: the battle compiler checks those).
   @param path  the battle manifest
   @return the records (structure order)
   @throws ScriptError when it cannot be read or a record is invalid
   """
   from battlec.common import ManifestError
   from battlec.field import FIELD_ACTIONS_KEY, compile_field_action_records
   from battlec.manifest import read_battle
   try:
      b, st_path = read_battle(Path(path), None, [])
      return compile_field_action_records(b.get(FIELD_ACTIONS_KEY), st_path.name)
   except ManifestError as e:
      raise ScriptError(f"cannot read the field actions of {path}: {e}") from None


def load_combine_methods(path: str | PathLike[str]) -> list[dict[str, Any]]:
   """!The combination methods of a battle manifest's structure (B1; tools/battlec/combine.py method_records(): names,
   kinds and input counts, without their rules: the battle compiler checks those), for the most generous platform.
   @param path  the battle manifest
   @return the records (structure order)
   @throws ScriptError when it cannot be read or a record is invalid
   """
   import build_settings
   from battlec.combine import COMBINE_KEY, method_records
   from battlec.common import ManifestError
   from battlec.manifest import read_battle
   try:
      b, st_path = read_battle(Path(path), None, [])
      top = max(v["max_combine_inputs"] for v in build_settings.PLATFORM_LIMITS.values())
      return method_records(b.get(COMBINE_KEY), top, st_path.name)
   except ManifestError as e:
      raise ScriptError(f"cannot read the combination methods of {path}: {e}") from None


## C10 (S1): parsed world and map manifests by (path, modification time, size): the field and the uses readers both
## read every map; a file read once in a run is parsed once (they only read what it holds).
_JSON_CACHE: dict[tuple[str, int, int], Any] = {}


def read_json_cached(path: Path) -> Any:
   """!C10: a JSON file, parsed once while it is unchanged (the result is shared: read it, never change it).
   @param path  the file
   @return the parsed value
   @throws OSError, ValueError as json.loads() / reading do
   """
   st = path.stat()
   key = (str(path.resolve()), st.st_mtime_ns, st.st_size)
   if key not in _JSON_CACHE:
      _JSON_CACHE[key] = json.loads(path.read_text(encoding="utf-8"))
   return _JSON_CACHE[key]


def load_world_field(path: str | PathLike[str]) -> dict[str, Any]:
   """!What the F2 statements and the field actions need of a world: its behaviors, object kinds and map tags (the
   targets), its flight points and whether any map records an escape point. Unreadable map manifests are skipped
   (the map compiler reports them).
   @param path  world manifest path
   @return {"behaviors", "object_kinds", "map_tags" (names in index order), "flight_points"
           (map_compiler.compile_flight_points()), "escape" (bool), C8: "darkness_levels" (the world's darkest
           level), "dark" (bool: a map starts dark), DS1: "override_sets" (every map's set names in the world's
           order: `override` statements' set ids), "override_persist" ([("flag" | "var", name)] the sets persist
           through: used names)}
   @throws ScriptError for a world whose lists are invalid
   """
   import map_compiler as mc
   path = Path(path)
   w = read_json_cached(path)
   if not isinstance(w, dict):
      raise ScriptError(f"{path.name}: not a world manifest")
   maps = w.get("maps", {}) if isinstance(w.get("maps"), dict) else {}
   try:
      out = {"behaviors": [str(b.get("name")) for b in w.get("behaviors", [{"name": "normal"}]) if isinstance(b, dict)],
             "object_kinds": mc.object_kinds(w), "map_tags": mc.map_tags(w),
             "flight_points": mc.compile_flight_points(w, list(maps)), "escape": False,
             "darkness_levels": len(mc.darkness_levels(w)), "dark": False}
   except mc.ManifestError as e:
      raise ScriptError(f"{path.name}: {e}") from None
   out["override_sets"] = []   # DS1: every map's override sets, in the world's order (map::kWorld.override_sets)
   out["override_persist"] = []   # DS1: ("flag" | "var", name) of the sets that persist
   for rel in maps.values():
      try:
         m = read_json_cached(path.parent / str(rel))
      except (OSError, ValueError):
         continue
      if isinstance(m, dict) and isinstance(m.get("override_sets"), list):
         sets = [s for s in m["override_sets"] if isinstance(s, dict)]
         out["override_sets"] += [str(s.get("name")) for s in sets]
         out["override_persist"] += [(kind, str(s["persist"][kind])) for s in sets
                                     if isinstance(s.get("persist"), dict) for kind in ("flag", "var")
                                     if kind in s["persist"]]
      if isinstance(m, dict) and m.get("escape_point") is True:
         out["escape"] = True
      if isinstance(m, dict) and isinstance(m.get("darkness"), int) and m["darkness"] > 0:
         out["dark"] = True   # C8: a dark map: the darkness variable is declared
   return out


def has_statement(items: Any, kind: str) -> bool:
   """!Whether parsed source items hold a statement of a kind anywhere (inside blocks too).
   @param items  Parser.file_items() result (or any part of it)
   @param kind   the statement's node kind
   @return True when one is found
   """
   # C10 (S1): one walk with a stack instead of a call per node (sources hold many thousand nodes)
   stack = [items]
   while stack:
      node = stack.pop()
      if isinstance(node, dict):
         if node.get("k") == kind:
            return True
         stack.extend(v for v in node.values() if isinstance(v, (dict, list)))
      elif isinstance(node, list):
         stack.extend(v for v in node if isinstance(v, (dict, list)))
   return False


def field_action_source(actions: Sequence[Mapping[str, Any]], world: Mapping[str, Any] | None,
                        dark: bool = False) -> str:
   """!The implicit declarations of F2, as `.cscript` source: the member variable (with field actions), each asking
   action's wrapper script (its prompt, yes / no, then its script), each flight point's flag, the escape point's
   variables (a world with escape points) and C8's darkness variable (DARKNESS_VAR).
   @param actions  load_field_actions() result
   @param world    load_world_field() result, or None
   @param dark     declare DARKNESS_VAR (a dark map, or scripts using `darkness`)
   @return the source
   """
   from battlec.field import ANSWER_VAR, MEMBER_VAR, ask_script_name, prompt_text
   lines = []
   if actions:
      lines.append(f"var {MEMBER_VAR} : u8 map;")
   if any(a["ask"] for a in actions):
      lines.append(f"var {ANSWER_VAR} : u8 map 0..1;")
   for a in actions:
      if a["ask"]:
         text = prompt_text(a).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
         lines.append(f'script {ask_script_name(a["name"])} {{ msg "{text}"; yesno -> var({ANSWER_VAR}); close_msg; '
                      f'if var({ANSWER_VAR}) == 1 {{ call {a["script"]}; }} }}')
   for fp in (world or {}).get("flight_points", []):
      lines.append(f"flag {FLIGHT_FLAG_PREFIX}{fp['name']} : {LIFETIMES[0]};")
   if (world or {}).get("escape"):
      for n in ESCAPE_VARS:
         lines.append(f"var {n} : u16 {LIFETIMES[0]};")
   if dark:
      # C8: a script-set darkness kept across a save until the next map (the map lifetime)
      lines.append(f"var {DARKNESS_VAR} : u8 {MAP_LIFETIME} 0..{MAX_DARKNESS + 1};")
   return "\n".join(lines) + "\n"


def field_effect_source(effects: Sequence[Mapping[str, Any]]) -> str:
   """!The implicit declarations of the timed field effects (F1), as `.cscript` source: per effect a permanent u16
   variable (what it has left) and, with an expiry message, a script that shows it.
   @param effects  load_field_effects() result
   @return the source
   """
   from battlec.field import VAR_TYPE, end_script_name, var_name
   lines = []
   for e in effects:
      lines.append(f"var {var_name(e['name'])} : {VAR_TYPE} {LIFETIMES[0]};")
      if e["expiry"] is not None:
         text = e["expiry"].replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
         lines.append(f'script {end_script_name(e["name"])} {{ msgbox "{text}"; close_msg; }}')
   return "\n".join(lines) + "\n"


def record_roles(layout: Mapping[str, Any] | None) -> tuple[dict[str, RoleShape], int]:
   """!The record roles scripts may name, and the party size.
   @param layout  load_record_layout() result, or None: the registry's engine roles (tools/battlec/layout.py
                  ROLES, retired roles left out) at their registry shapes, parties of PARTY_SIZE
   @return ({name: (role ID, width, count)}, party size)
   """
   from battlec.layout import RETIRED_ROLES, ROLES, role_info
   if layout is None:
      return {name: (rid, width, count) for rid, (name, width, count, _rule) in enumerate(ROLES)
              if name not in RETIRED_ROLES}, PARTY_SIZE
   return {name: role_info(layout, name) for name in layout["roles"]}, layout["party_size"]


class FieldPlaceholders:
   """!A scripts manifest's field placeholders and terms (D-22 decision 6), checked for shape.

   Shared by the script compiler (inline texts, the bank's placeholder table)
   and the map compiler (location names): both encode field text with it.
   """

   def __init__(self, manifest: Mapping[str, Any],
                codepage: text_encoder.CodePage = text_encoder.DEFAULT,
                layout: Mapping[str, Any] | None = None) -> None:
      """!Reads "placeholders" and "terms".
      @param manifest  the capsule.scripts/1 manifest
      @param codepage  the game's code page (term texts)
      @param layout    the game's record layout (load_record_layout() of its "battle"), or None
      @throws ScriptError when either is malformed
      """
      roles, party_size = record_roles(layout)
      ## Record roles by name: (role ID, width, count).
      self.roles = roles
      ## Party slots a record binding may name.
      self.party_size = party_size
      ## Whether the manifest declares placeholders: field texts are markup.
      self.declared = "placeholders" in manifest
      ## The code page.
      self.codepage = codepage
      raw = manifest.get("placeholders", {})
      if not isinstance(raw, Mapping):
         raise ScriptError("placeholders: an object of name -> binding")
      terms = manifest.get("terms")
      if terms is not None and (not isinstance(terms, Mapping) or not all(
            isinstance(v, str) or (isinstance(v, Mapping) and isinstance(v.get(TERM_ONE), str))
            for v in terms.values())):
         raise ScriptError("terms: an object of concept -> \"text\" or {\"one\": text, \"many\": text}")
      ## Terms (concept -> text) for term markup and term bindings, or None.
      self.terms = dict(terms) if terms is not None else None
      try:
         ## Placeholder name -> argument byte after FD.
         self.codes = text_encoder.field_placeholders(raw, "placeholders")
      except text_encoder.TextError as e:
         raise ScriptError(str(e)) from None
      ## Placeholder name -> (kind, value, extra keys).
      self.bindings = {}
      ## Placeholder name -> most cells it prints.
      self.widths = {}
      for name, b in raw.items():
         where = f"placeholder {{{name}}}"
         kinds = [k for k in PLACEHOLDER_KINDS if isinstance(b, Mapping) and k in b]
         if len(kinds) != 1:
            raise ScriptError(f"{where}: a binding with one of {', '.join(PLACEHOLDER_KINDS)}")
         kind = kinds[0]
         unknown = sorted(set(b) - {kind} - PLACEHOLDER_EXTRA_KEYS[kind])
         if unknown:
            raise ScriptError(f"{where}: unknown keys {unknown}")
         self.bindings[name] = (kind, b[kind], {k: b[k] for k in b if k != kind})
         self.widths[name] = self._width(where, kind, b[kind], b)

   def _width(self, where: str, kind: str, value: Any, binding: Mapping[str, Any]) -> int:
      """!Checks one binding's values and gives the most cells it prints.
      @param where    location for error messages
      @param kind     its kind
      @param value    the kind key's value
      @param binding  the whole binding
      @return cells
      """
      limit = text_encoder.MAX_PLACEHOLDER_CODES
      if kind == "name":
         if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= limit:
            raise ScriptError(f"{where}: a name slot holds 1-{limit} characters")
         return value
      if kind == "var":
         if not isinstance(value, str):
            raise ScriptError(f"{where}: var names a declared variable")
         return MAX_VAR_DIGITS
      if kind == "term":
         return len(self.term_codes(where, value, binding.get("form", TERM_ONE))) - 1
      if value not in self.roles:
         raise ScriptError(f"{where}: record names a record role ({', '.join(self.roles)})")
      slot = binding.get("slot", 0)
      if isinstance(slot, bool) or not isinstance(slot, int) or not 0 <= slot < self.party_size:
         raise ScriptError(f"{where}: slot is a party slot, 0-{self.party_size - 1}")
      text_codes = self.text_codes(value)
      if text_codes:
         return text_codes
      return len(str(min((1 << self.roles[value][1]) - 1, MAX_VAR_VALUE)))

   def text_codes(self, role: str) -> int:
      """!How many text codes a record binding of a role prints (Placeholder::text of a Record binding).
      @param role  a role name of self.roles
      @return codes, or 0 for a number (printed in decimal)
      """
      _rid, width, count = self.roles[role]
      return min(count, text_encoder.MAX_PLACEHOLDER_CODES) if width == TEXT_ROLE_WIDTH and count > 1 else 0

   def term_codes(self, where: str, concept: Any, form: Any) -> list[int]:
      """!The codes of a term binding's text.
      @param where    location for error messages
      @param concept  the term's concept
      @param form     "one" or "many"
      @return glyph codes and CODE_END
      """
      if not isinstance(concept, str) or concept not in (self.terms or {}):
         raise ScriptError(f"{where}: term names a concept of \"terms\" ({', '.join(self.terms or {}) or 'none'})")
      if form not in (TERM_ONE, TERM_MANY):
         raise ScriptError(f"{where}: form is {TERM_ONE} or {TERM_MANY}")
      term = self.terms[concept]
      text = term if isinstance(term, str) else term.get(form)
      if isinstance(term, str) and form == TERM_MANY:
         text = None
      if not isinstance(text, str):   # I4: an empty term is a term (an honorific a language leaves out)
         raise ScriptError(f"{where}: term {concept!r} has no {form!r} form")
      try:
         codes = self.codepage.encode(text, where, markup=False)
      except text_encoder.TextError as e:
         raise ScriptError(str(e)) from None
      if len(codes) - 1 > text_encoder.MAX_PLACEHOLDER_CODES:
         raise ScriptError(f"{where}: the term's text is longer than {text_encoder.MAX_PLACEHOLDER_CODES} characters")
      return codes

   def encode(self, text: Any, where: str) -> list[int]:
      """!Encodes a field text (only when the manifest declares placeholders: markup on).
      @param text   the text
      @param where  location for error messages
      @return the codes, ending with CODE_END
      @throws text_encoder.TextError when the text cannot be encoded
      """
      return text_encoder.encode_field(text, where, self.codes, self.terms, codepage=self.codepage)


def load_placeholders(path: str | PathLike[str] | None,
                      codepage: text_encoder.CodePage = text_encoder.DEFAULT) -> FieldPlaceholders | None:
   """!The field placeholders a scripts manifest declares (for compilers of other field texts).
   @param path      the scripts manifest, or None
   @param codepage  the game's code page
   @return them, or None when there is no manifest or it declares none (field texts keep braces as characters)
   @throws ScriptError when they are malformed
   """
   if path is None:
      return None
   try:
      manifest = json.loads(Path(path).read_text(encoding="utf-8"))
   except (OSError, ValueError) as e:
      raise ScriptError(f"cannot read {path}: {e}") from None
   if not isinstance(manifest, Mapping) or "placeholders" not in manifest:
      return None
   layout = None
   if isinstance(manifest.get("battle"), str):
      layout = load_record_layout(Path(path).parent / manifest["battle"])
   return FieldPlaceholders(manifest, codepage, layout)


class ScriptError(Exception):
   """!A source or manifest error at a position."""

   def __init__(self, message: str, file: str = "", line: int = 0, col: int = 0) -> None:
      """!Creates the error.
      @param message  what is wrong
      @param file     source file ('' for the manifest)
      @param line     1-based line, 0 when unknown
      @param col      1-based column, 0 when unknown
      """
      super().__init__(message)
      ## What is wrong.
      self.message = message
      ## Source file.
      self.file = file
      ## 1-based line (0 when unknown).
      self.line = line
      ## Column.
      self.col = col

   def __str__(self) -> str:
      """!The error as 'file:line:col: message'.
      @return the text
      """
      where = self.file or "manifest"
      if self.line != 0:
         where += f":{self.line}:{self.col}"
      return f"{where}: {self.message}"


class Diagnostic:
   """!One error or warning, for the command line and the editor."""

   def __init__(self, severity: str, message: str, file: str = "", line: int = 0, col: int = 0) -> None:
      """!Creates a diagnostic.
      @param severity  'error' or 'warning'
      @param message   text
      @param file      source file
      @param line      1-based line (0 = none)
      @param col       1-based column
      """
      ## 'error' or 'warning'.
      self.severity = severity
      ## Text.
      self.message = message
      ## Source file.
      self.file = file
      ## 1-based line (0 when unknown).
      self.line = line
      ## Column.
      self.col = col

   def __str__(self) -> str:
      """!The diagnostic as 'file:line:col: severity: message'.
      @return the text
      """
      where = self.file or "manifest"
      if self.line != 0:
         where += f":{self.line}:{self.col}"
      return f"{where}: {self.severity}: {self.message}"


# --- lexer ---------------------------------------------------------------------

## Language words, as a regex alternation (a reference's namespace or name is never one, so
## `x:u8`, `f:map` and `top:goto top;` still read as declarations and labels).
_WORDS_RE = "|".join(sorted(KEYWORDS, key=len, reverse=True))
## Token pattern: comments, strings, namespaced references, numbers, words, symbols.
TOKEN_RE = re.compile(r"""
    (?P<ws>[ \t\r\n]+) |
    (?P<line_comment>//[^\n]*) |
    (?P<block_comment>/\*.*?\*/) |
    (?P<string>"(?:[^"\\\n]|\\.)*") |
    (?P<ref>(?!(?:WORDS)\b)[A-Za-z_][A-Za-z0-9_]*:
        (?:"(?:[^"\\\n]|\\.)*"|(?!(?:WORDS)(?![A-Za-z0-9_.'-]))[A-Za-z0-9_][A-Za-z0-9_.'-]*)) |
    (?P<number>-?(?:0[xX][0-9A-Fa-f]+|[0-9]+)) |
    (?P<name>[A-Za-z_][A-Za-z0-9_]*) |
    (?P<sym>->|==|!=|<=|>=|\+=|-=|\*=|/=|%=|&&|\|\||\.\.|[{}()\[\];,:=<>!])
""".replace("WORDS", _WORDS_RE), re.VERBOSE | re.DOTALL)


def parse_int(text: str) -> int:
   """!Value of a number token: decimal (leading zeros allowed) or 0x hexadecimal.
   @param text  the token
   @return int
   """
   body = text[1:] if text.startswith("-") else text
   is_hex = body[:HEX_PREFIX_LENGTH] in ("0x", "0X")
   v = int(body[HEX_PREFIX_LENGTH:], HEX_BASE) if is_hex else int(body, DECIMAL_BASE)
   return -v if text.startswith("-") else v


class Token:
   """!One token with its position."""

   ## C10 (S1): fixed attributes (faster to read, smaller: sources make hundreds of thousands of tokens).
   __slots__ = ("kind", "text", "line", "col")

   def __init__(self, kind: str, text: str, line: int, col: int) -> None:
      """!Creates a token.
      @param kind  'string', 'ref', 'number', 'name', 'sym' or 'eof'
      @param text  the source text (strings: their decoded value)
      @param line  1-based line
      @param col   1-based column
      """
      ## Kind.
      self.kind = kind
      ## Text.
      self.text = text
      ## 1-based line (0 when unknown).
      self.line = line
      ## Column.
      self.col = col

   def __repr__(self) -> str:
      """!Debug form.
      @return text
      """
      return f"{self.kind}:{self.text!r}@{self.line}:{self.col}"


def unescape(body: str, file: str, line: int, col: int) -> str:
   """!Decodes a string literal's escapes (\\n, \\", \\\\).
   @param body  text between the quotes
   @param file  source file
   @param line  line of the literal
   @param col   column of the literal
   @return the string
   """
   out = []
   i = 0
   while i < len(body):
      c = body[i]
      if c == "\\":
         i += 1
         e = body[i] if i < len(body) else ""
         if e == "n":
            out.append("\n")
         elif e in "\"\\":
            out.append(e)
         else:
            raise ScriptError(f"unknown escape \\{e} in a string", file, line, col)
      else:
         out.append(c)
      i += 1
   return "".join(out)


def tokenize(text: str, file: str) -> list[Token]:
   """!Splits a source into tokens.
   @param text  the source
   @param file  its name, for errors
   @return list of Token, ending with an 'eof' token
   """
   out = []
   pos = 0
   line = 1
   line_start = 0
   while pos < len(text):
      m = TOKEN_RE.match(text, pos)
      if not m:
         raise ScriptError(f"unexpected character {text[pos]!r}", file, line, pos - line_start + 1)
      kind = m.lastgroup
      value = m.group()
      col = pos - line_start + 1
      if kind == "string":
         out.append(Token("string", unescape(value[1:-1], file, line, col), line, col))
      elif kind == "ref":
         ns, rest = value.split(":", 1)
         if rest.startswith('"'):
            rest = unescape(rest[1:-1], file, line, col)
         out.append(Token("ref", f"{ns}:{rest}", line, col))
      elif kind in ("number", "name", "sym"):
         out.append(Token(kind, value, line, col))
      # Track the line and where it starts, for token columns.
      newlines = value.count("\n")
      if newlines:
         line += newlines
         line_start = pos + value.rfind("\n") + 1
      pos = m.end()
   out.append(Token("eof", "", line, pos - line_start + 1))
   return out


# --- parser --------------------------------------------------------------------

class Node(dict):
   """!A syntax-tree node: a dict with 'k' (kind) and its position."""

   def __init__(self, node_kind: str, tok: Token, **fields: Any) -> None:
      """!Creates a node.
      @param node_kind  node kind
      @param tok        token it starts at (for its position)
      @param fields     kind-specific fields
      """
      super().__init__(k=node_kind, line=tok.line, col=tok.col, **fields)


## C10 (S1): the token kinds at() / accept() match by their text.
WORD_KINDS = ("name", "sym")


class Parser:
   """!Recursive-descent parser for one source file."""

   def __init__(self, text: str, file: str) -> None:
      """!Tokenizes a source.
      @param text  the source
      @param file  its name
      """
      ## Source name.
      self.file = file
      ## Tokens.
      self.toks = tokenize(text, file)
      ## Read position.
      self.i = 0
      ## C10 (S1): the last token's index (the eof token: reading past it reads it).
      self.last = len(self.toks) - 1

   def peek(self, ahead: int = 0) -> Token:
      """!A token without consuming it.
      @param ahead  0 = the next token
      @return the token
      """
      j = self.i + ahead
      return self.toks[j if j < self.last else self.last]

   def error(self, msg: str, tok: Token | None = None) -> ScriptError:
      """!A ScriptError at a token.
      @param msg  text
      @param tok  token (default: the next one)
      @return the error (to raise)
      """
      tok = tok or self.peek()
      return ScriptError(msg, self.file, tok.line, tok.col)

   def take(self) -> Token:
      """!Consumes the next token.
      @return it
      """
      t = self.toks[self.i if self.i < self.last else self.last]
      self.i += 1
      return t

   def at(self, text: str) -> bool:
      """!Whether the next token is a given word or symbol.
      @param text  the word or symbol
      @return true when it is
      """
      t = self.toks[self.i if self.i < self.last else self.last]
      return t.text == text and t.kind in WORD_KINDS

   def accept(self, text: str) -> Token | None:
      """!Consumes the next token if it is a given word or symbol.
      @param text  the word or symbol
      @return the token, or None
      """
      t = self.toks[self.i if self.i < self.last else self.last]
      if t.text == text and t.kind in WORD_KINDS:
         self.i += 1
         return t
      return None

   def expect(self, text: str) -> Token:
      """!Consumes a required word or symbol.
      @param text  the word or symbol
      @return the token
      """
      if not self.at(text):
         t = self.peek()
         raise self.error(f"expected '{text}', found {t.text or 'end of file'!r}")
      return self.take()

   def name(self, what: str = "a name") -> Token:
      """!Consumes an identifier that is not a keyword.
      @param what  what it names, for the error
      @return its token
      """
      t = self.peek()
      if t.kind != "name" or t.text in KEYWORDS:
         raise self.error(f"expected {what}, found {t.text or 'end of file'!r}")
      return self.take()

   def number(self) -> tuple[int, Token]:
      """!Consumes an integer literal.
      @return (value, token)
      """
      t = self.peek()
      if t.kind != "number":
         raise self.error(f"expected a number, found {t.text or 'end of file'!r}")
      self.take()
      return parse_int(t.text), t

   # Top level.

   def file_items(self) -> list[Node]:
      """!Parses a whole file.
      @return list of declaration nodes
      """
      items = []
      while self.peek().kind != "eof":
         items.append(self.declaration())
      return items

   def declaration(self) -> Node:
      """!Parses one top-level declaration.
      @return its node
      """
      t = self.peek()
      if self.accept("flag"):
         n = self.name("a flag name")
         life = "permanent"
         if self.accept(":"):
            life = self.lifetime()
         self.expect(";")
         return Node("flag", t, name=n.text, lifetime=life)
      if self.accept("var"):
         n = self.name("a variable name")
         self.expect(":")
         ty = self.peek()
         if ty.text not in TYPES:
            raise self.error("expected a type: u8, u16 or u32")
         self.take()
         life = "permanent"
         if self.peek().text in LIFETIMES:
            life = self.lifetime()
         rng = None
         if self.peek().kind == "number":
            lo, _ = self.number()
            self.expect("..")
            hi, _ = self.number()
            rng = (lo, hi)
         self.expect(";")
         return Node("var", t, name=n.text, type=ty.text, lifetime=life, range=rng)
      if self.accept("const"):
         n = self.name("a constant name")
         self.expect("=")
         v, _ = self.number()
         self.expect(";")
         return Node("const", t, name=n.text, value=v)
      if self.accept("native"):
         n = self.name("a native name")
         self.expect("(")
         params = []
         if not self.at(")"):
            params.append(self.name("a parameter name").text)
            while self.accept(","):
               params.append(self.name("a parameter name").text)
         self.expect(")")
         returns = False
         waits = False
         if self.accept("->"):
            self.expect("value")
            returns = True
         if self.accept("waits"):
            waits = True
         self.expect(";")
         return Node("native", t, name=n.text, params=params, returns=returns, waits=waits)
      if self.accept("script"):
         n = self.name("a script name")
         return Node("script", t, name=n.text, body=self.block())
      if self.accept("map_scripts"):
         n = self.name("a map name")
         self.expect("{")
         entries = []
         while not self.accept("}"):
            e = self.peek()
            kind = e.text
            if kind not in TRIGGER_KINDS:
               raise self.error(f"expected {', '.join(TRIGGER_KINDS)}")
            self.take()
            cond = self.condition() if kind in CONDITION_KINDS else None
            self.expect("->")
            target = self.name("a script name")
            self.expect(";")
            entries.append(Node("trigger", e, kind=kind, cond=cond, script=target.text))
         return Node("map_scripts", t, name=n.text, entries=entries)
      if self.at("when"):
         neg, games, glitch = self.when_head()
         body = self.declaration_block()
         other = self.declaration_block() if self.accept("else") else None
         return Node("when", t, negate=neg, games=games, glitch=glitch, body=body, other=other)
      # I4: a named move route, a mart list, a game route step (their words are no keywords)
      if self.at(MOVEMENT_WORD) and self.peek(1).kind == "name" and self.peek(DECLARATION_AHEAD).text == "{":
         self.take()
         n = self.name("a movement name")
         return Node("movement", t, name=n.text, steps=self.route_body())
      if self.at(MART_WORD) and self.peek(1).kind == "name" and self.peek(DECLARATION_AHEAD).text == "{":
         self.take()
         n = self.name("a mart name")
         self.expect("{")
         items = []
         if not self.at("}"):
            items.append(self.operand())
            while self.accept(","):
               items.append(self.operand())
         self.expect("}")
         return Node("mart", t, name=n.text, items=items)
      if self.at(STEP_WORD) and self.peek(1).kind == "name" and self.peek(DECLARATION_AHEAD).text == ";":
         self.take()
         n = self.name("a route step name")
         self.expect(";")
         return Node("step", t, name=n.text)
      raise self.error(f"expected a declaration (flag, var, const, native, script, map_scripts, when, movement, "
                       f"mart, step), found {t.text or 'end of file'!r}")

   def declaration_block(self) -> list[Node]:
      """!Parses '{ declarations }' (a when block's body).
      @return the declaration nodes
      """
      self.expect("{")
      body = []
      while not self.accept("}"):
         body.append(self.declaration())
      return body

   def route_body(self) -> list[list[str]]:
      """!Parses a route's '{ step; step; }': each step's words.
      @return the steps
      """
      self.expect("{")
      steps = []
      while not self.accept("}"):
         words = self.words_until(";")
         self.accept(";")
         if words:
            steps.append(words)
      return steps

   def lifetime(self) -> str:
      """!Parses a lifetime word.
      @return its name
      """
      t = self.peek()
      if t.text not in LIFETIMES:
         raise self.error("expected a lifetime: permanent, daily, area or map")
      self.take()
      return t.text

   def when_head(self) -> tuple[bool, list[str], str | None]:
      """!Parses 'when [not] game(a, b)' or (I4) 'when [not] glitch("id")'.
      @return (negated, [game names], the glitch id or None)
      """
      self.expect("when")
      neg = bool(self.accept("not"))
      if self.at(GLITCH_WORD) and self.peek(1).text == "(":
         self.take()
         self.expect("(")
         t = self.peek()
         if t.kind != "string" or not t.text:
            raise self.error("glitch(...) names a glitch id as a string")
         self.take()
         self.expect(")")
         return neg, [], t.text
      self.expect("game")
      self.expect("(")
      games = [self.name("a game name").text]
      while self.accept(","):
         games.append(self.name("a game name").text)
      self.expect(")")
      return neg, games, None

   # Statements.

   def block(self) -> list[Node]:
      """!Parses '{ statements }'.
      @return list of statement nodes
      """
      self.expect("{")
      out = []
      while not self.accept("}"):
         if self.peek().kind == "eof":
            raise self.error("missing '}'")
         out.append(self.statement())
      return out

   def var_ref(self) -> Token:
      """!Parses 'var(name)'.
      @return the variable name token
      """
      self.expect("var")
      self.expect("(")
      n = self.name("a variable name")
      self.expect(")")
      return n

   def flag_ref(self) -> Token:
      """!Parses 'flag(name)'.
      @return the flag name token
      """
      self.expect("flag")
      self.expect("(")
      n = self.name("a flag name")
      self.expect(")")
      return n

   def operand(self) -> Node:
      """!Parses an operand.
      @return its node: const (value), constname, var, or ref
      """
      t = self.peek()
      if t.kind == "number":
         v, _ = self.number()
         return Node("num", t, value=v)
      if t.kind == "ref":
         self.take()
         return Node("ref", t, ref=t.text)
      if self.at("var") and self.peek(1).text == "(":
         n = self.var_ref()
         return Node("varop", t, name=n.text)
      if self.at("script") and self.peek(1).text == "(":
         # I4: a script's ID as a constant (a native's callback)
         self.take()
         self.expect("(")
         n = self.name("a script name")
         self.expect(")")
         return Node("scriptref", t, name=n.text)
      if t.text in QUERIES:
         self.take()
         obj = None
         if QUERIES[t.text]:
            self.expect("(")
            obj = self.object_ref()
            self.expect(")")
         return Node("query", t, query=t.text, obj=obj)
      if t.text in ("yes", "true"):
         self.take()
         return Node("num", t, value=1)
      if t.text in ("no", "false"):
         self.take()
         return Node("num", t, value=0)
      if t.kind == "name" and t.text not in KEYWORDS:
         self.take()
         return Node("constname", t, name=t.text)
      raise self.error(f"expected a value (number, constant, var(...), yes / no), found {t.text!r}")

   def object_ref(self) -> Node:
      """!Parses a map object: self, player or an operand.
      @return its node (num for self / player, marked with 'obj')
      """
      t = self.peek()
      if self.accept("self"):
         return Node("num", t, value=OBJECT_SELF, obj="self")
      if self.accept("player"):
         return Node("num", t, value=OBJECT_PLAYER, obj="player")
      if t.kind == "name" and t.text in OBJECT_WORDS and self.peek(1).text != "(":
         # I4: the camera, the object last given a route (words, not keywords)
         self.take()
         return Node("num", t, value=OBJECT_WORDS[t.text], obj=t.text)
      return self.operand()

   def self_ref(self) -> str:
      """!Parses 'self(A)' (a self switch, A-D).
      @return the letter
      """
      self.expect("self")
      self.expect("(")
      n = self.name("a self switch letter (A-D)")
      if n.text not in SELF_LETTERS or len(n.text) != 1:
         raise self.error(f"self switches are A, B, C and D, not {n.text!r}", n)
      self.expect(")")
      return n.text

   def words_until(self, end: str) -> list[str]:
      """!Collects the texts of tokens up to (not including) @p end.
      @param end  the closing symbol
      @return list of words
      """
      words = []
      while not self.at(end) and not self.at("}"):
         t = self.take()
         if t.kind == "eof":
            raise self.error(f"expected {end!r} before the end of the file", t)
         words.append(t.text)
      return words

   def direction(self, allowed: Sequence[str]) -> str:
      """!Parses a direction word.
      @param allowed  the words allowed here
      @return the word
      """
      t = self.peek()
      if t.text not in allowed:
         raise self.error(f"expected {', '.join(allowed)}, found {t.text or 'end of file'!r}")
      self.take()
      return t.text

   def text_ref(self) -> Node:
      """!Parses a text: a string literal or text:key.
      @return its node
      """
      t = self.peek()
      if t.kind == "string":
         self.take()
         return Node("inline", t, text=t.text)
      if t.kind == "ref" and t.text.startswith("text:"):
         self.take()
         return Node("textkey", t, ref=t.text)
      raise self.error("expected a text: \"...\" or text:key")

   def condition(self) -> Node:
      """!Parses a condition (|| of && of unary).
      @return its node
      """
      left = self.cond_and()
      while self.at("||"):
         t = self.take()
         left = Node("or", t, a=left, b=self.cond_and())
      return left

   def cond_and(self) -> Node:
      """!Parses a chain of &&.
      @return its node
      """
      left = self.cond_unary()
      while self.at("&&"):
         t = self.take()
         left = Node("and", t, a=left, b=self.cond_unary())
      return left

   def cond_unary(self) -> Node:
      """!Parses !, (cond), flag(...) or a comparison.
      @return its node
      """
      t = self.peek()
      if self.accept("!"):
         return Node("not", t, a=self.cond_unary())
      if self.accept("("):
         c = self.condition()
         self.expect(")")
         return c
      if self.at("flag"):
         n = self.flag_ref()
         return Node("flagc", t, name=n.text)
      if self.at("self") and self.peek(1).text == "(":
         return Node("selfc", t, letter=self.self_ref())
      a = self.operand()
      op = self.peek()
      if op.text not in COMPARES:
         raise self.error("expected a comparison (== != < <= > >=)")
      self.take()
      return Node("cmp", op, op=op.text, a=a, b=self.operand())

   def f2_statement(self) -> Node | None:
      """!Parses an F2 statement (D-27 section 3) when one starts here: `darkness <operand>;`, `flight record|visit
      NAME [transition];`, `flight menu -> var(v);`, `escape [transition] [-> var(ok)];`, `field_action available
      NAME -> var(v);`. Their first words are no keywords (a script may still name a label or a variable so).
      @return its node, or None (another statement)
      """
      t = self.peek()
      nxt = self.peek(1)
      if self.at(DARKNESS_WORD) and (nxt.kind == "number" or nxt.text == "var" or
                                     (nxt.kind == "name" and self.peek(OPERAND_END_AHEAD).text == ";")):
         self.take()
         level = self.operand()
         self.expect(";")
         return Node("darkness", t, level=level)
      if self.at(FLIGHT_WORD) and nxt.text in FLIGHT_KINDS:
         self.take()
         kind = self.take().text
         if kind == "menu":
            if not self.accept("->"):
               raise self.error("flight menu needs -> var(v)", t)
            n = self.var_ref()
            self.expect(";")
            return Node("flight", t, kind=kind, point=None, transition="default", result=n.text)
         point = self.name("a flight point name").text
         trans = "default"
         if kind == "visit" and self.peek().text in TRANSITIONS:
            trans = self.take().text
         self.expect(";")
         return Node("flight", t, kind=kind, point=point, transition=trans, result=None)
      if self.at(ESCAPE_WORD) and (nxt.text in (";", "->") or nxt.text in TRANSITIONS):
         self.take()
         trans = self.take().text if self.peek().text in TRANSITIONS else "default"
         result = self.var_ref().text if self.accept("->") else None
         self.expect(";")
         return Node("escape", t, transition=trans, result=result)
      if self.at(FIELD_ACTION_WORD) and nxt.text in FIELD_ACTION_KINDS:
         self.take()
         kind = self.take().text
         name = self.name("a field action name").text
         if not self.accept("->"):
            raise self.error(f"field_action {kind} needs -> var(v)", t)
         n = self.var_ref()
         self.expect(";")
         return Node("field_action", t, kind=kind, name=name, result=n.text)
      if self.at(NURSERY_WORD) and nxt.text in NURSERY_KINDS:
         # B2: nursery KIND [<operand>] [pay var(money)] -> var(v);
         self.take()
         kind = self.take().text
         arg = self.operand() if kind in NURSERY_OPERAND else None
         money = None
         if kind == "withdraw" and self.accept(PAY_WORD):
            money = self.var_ref().text
         if not self.accept("->"):
            raise self.error(f"nursery {kind} needs -> var(v)", t)
         n = self.var_ref()
         self.expect(";")
         return Node("nursery", t, kind=kind, arg=arg, money=money, result=n.text)
      if self.at(COMBINE_WORD) and nxt.text in COMBINE_KINDS:
         # B1: combine KIND NAME [(<member>, ...)] -> var(v);
         self.take()
         kind = self.take().text
         name = self.name("a combination method name").text
         members: list[Node] = []
         if kind != "open":
            self.expect("(")
            members.append(self.operand())
            while self.accept(","):
               members.append(self.operand())
            self.expect(")")
         if not self.accept("->"):
            raise self.error(f"combine {kind} needs -> var(v)", t)
         n = self.var_ref()
         self.expect(";")
         return Node("combine", t, kind=kind, name=name, members=members, result=n.text)
      return None

   def statement(self) -> Node:
      """!Parses one statement.
      @return its node
      """
      t = self.peek()
      if t.kind == "name" and self.peek(1).text == ":" and t.text not in KEYWORDS:
         self.take()
         self.take()
         return Node("label", t, name=t.text)
      if self.accept("goto"):
         n = self.name("a label")
         self.expect(";")
         return Node("goto", t, label=n.text)
      if self.accept("call"):
         if self.at("var") and self.peek(1).text == "(":
            sel = Node("varop", self.peek(), name=self.var_ref().text)
            self.expect("of")
            self.expect("[")
            targets = [self.name("a script name").text]
            while self.accept(","):
               targets.append(self.name("a script name").text)
            self.expect("]")
            self.expect(";")
            return Node("calltable", t, sel=sel, targets=targets)
         n = self.name("a script name")
         loose = self.peek().kind == "name" and self.peek().text == LOOSE_WORD
         if loose:
            self.take()   # I4: a call that may recurse (CallLoose)
         self.expect(";")
         return Node("call", t, script=n.text, loose=loose)
      if self.accept("return"):
         self.expect(";")
         return Node("return", t)
      if self.accept("end"):
         self.expect(";")
         return Node("end", t)
      if self.accept("if"):
         cond = self.condition()
         then = self.block()
         other = None
         if self.accept("else"):
            other = [self.statement()] if self.at("if") else self.block()
         return Node("if", t, cond=cond, then=then, other=other)
      if self.accept("while"):
         cond = self.condition()
         return Node("while", t, cond=cond, body=self.block())
      if self.accept("switch"):
         value = self.operand()
         self.expect("{")
         cases = []
         default = None
         while not self.accept("}"):
            c = self.peek()
            if self.accept("case"):
               values = [self.operand()]
               while self.accept(","):
                  values.append(self.operand())
               cases.append(Node("case", c, values=values, body=self.block()))
            elif self.accept("default"):
               if default is not None:
                  raise self.error("a switch has one default")
               default = self.block()
            else:
               raise self.error("expected 'case' or 'default'")
         return Node("switch", t, value=value, cases=cases, default=default)
      if self.accept("set") or self.at("clear"):
         on = t.text == "set"
         if not on:
            self.take()
         if self.at("self"):
            letter = self.self_ref()
            self.expect(";")
            return Node("setself", t, letter=letter, on=on)
         n = self.flag_ref()
         self.expect(";")
         return Node("setflag", t, name=n.text, on=on)
      if self.at("var") and self.peek(1).text == "(":
         n = self.var_ref()
         op = self.peek()
         if op.text not in ("=", "+=", "-=", "*=", "/=", "%="):
            raise self.error("expected =, +=, -=, *=, /= or %=")
         self.take()
         if op.text == "=" and self.at("random"):
            self.take()
            self.expect("(")
            bound = self.operand()
            self.expect(")")
            self.expect(";")
            return Node("random", t, name=n.text, bound=bound)
         value = self.operand()
         self.expect(";")
         return Node("assign", t, name=n.text, op=op.text, value=value)
      if self.at("msg") or self.at("msgbox"):
         box = self.take().text == "msgbox"
         kind = None
         if box and self.peek().text in MSGBOX_KINDS and self.peek(1).kind in ("string", "ref"):
            kind = self.take().text   # I4: a message box kind
         text = self.text_ref()
         flags = 0
         while not box and self.peek().kind == "name" and self.peek().text in MSG_FLAGS:
            flags |= MSG_FLAGS[self.take().text]   # I4: msg modifiers
         result = None
         if kind == "yesno":
            if not self.accept("->"):
               raise self.error("msgbox yesno needs -> var(v)", t)
            result = self.var_ref().text
         self.expect(";")
         return Node("msg", t, text=text, box=box, kind=kind, flags=flags, result=result)
      if self.accept("wait_button"):
         self.expect(";")
         return Node("wait_button", t)
      if self.accept("close_msg"):
         self.expect(";")
         return Node("close_msg", t)
      if self.accept("yesno"):
         self.expect("->")
         n = self.var_ref()
         self.expect(";")
         return Node("yesno", t, name=n.text)
      if self.accept("choice"):
         self.expect("[")
         options = [self.text_ref()]
         while self.accept(","):
            options.append(self.text_ref())
         self.expect("]")
         cancel = None
         if self.accept("cancel"):
            cancel, _ = self.number()
         self.expect("->")
         n = self.var_ref()
         self.expect(";")
         return Node("choice", t, options=options, cancel=cancel, name=n.text)
      if self.accept("delay"):
         frames = self.operand()
         self.expect(";")
         return Node("delay", t, frames=frames)
      if self.accept("native"):
         n = self.name("a native name")
         self.expect("(")
         args = []
         if not self.at(")"):
            args.append(self.operand())
            while self.accept(","):
               args.append(self.operand())
         self.expect(")")
         result = None
         if self.accept("->"):
            result = self.var_ref().text
         self.expect(";")
         return Node("native", t, name=n.text, args=args, result=result)
      if self.at("move") or self.at("face") or self.at("hide") or self.at("show"):
         kind = self.take().text
         obj = self.object_ref()
         if kind == "move":
            d = self.direction(("down", "up", "left", "right"))
            at_end = self.at(";") or self.at(CHECKED_WORD)
            steps = Node("num", self.peek(), value=1) if at_end else self.operand()
            # C8: `checked [-> var(ok)]`: the walk by the field's movement rules, or none
            checked = self.accept(CHECKED_WORD)
            result = self.var_ref().text if checked and self.accept("->") else None
            self.expect(";")
            return Node("move", t, obj=obj, dir=d, steps=steps, checked=bool(checked), result=result)
         if kind == "face":
            d = self.direction(("down", "up", "left", "right", *FACE_WORDS))
            self.expect(";")
            return Node("face", t, obj=obj, dir=d)
         self.expect(";")
         return Node("showobj", t, obj=obj, on=kind == "show")
      if self.accept("route"):
         obj = self.object_ref()
         flags = set()
         while self.peek().text in ("repeat", "skip", "nowait"):
            flags.add(self.take().text)
         movement = None
         if self.peek().kind == "name" and not self.at("{"):
            # I4: a named movement (`movement NAME { ... }`), then its flags
            movement = self.name("a movement name").text
            while self.peek().text in ("repeat", "skip", "nowait"):
               flags.add(self.take().text)
            self.expect(";")
            steps = []
         else:
            steps = self.route_body()
            self.accept(";")
         return Node("route", t, obj=obj, steps=steps, movement=movement, repeat="repeat" in flags,
                     skip="skip" in flags, nowait="nowait" in flags or "repeat" in flags)
      if self.accept("wait_moves"):
         obj = None if self.at(";") else self.object_ref()   # I4: one object, or the last one given a route
         self.expect(";")
         return Node("wait_moves", t, obj=obj)
      if self.accept("input_number"):
         digits, _ = self.number()
         self.expect("->")
         n = self.var_ref()
         self.expect(";")
         return Node("input_number", t, digits=digits, name=n.text)
      if self.accept("wait_key"):
         self.expect("->")
         n = self.var_ref()
         self.expect(";")
         return Node("wait_key", t, name=n.text)
      if self.accept("msg_pos"):
         pos = self.direction(MSG_POSITIONS)
         self.expect(";")
         return Node("msg_pos", t, pos=pos)
      if self.accept("warp"):
         m = self.name("a map name")
         x = self.operand()
         y = self.operand()
         facing, trans = None, "default"
         if self.peek().text in ("down", "up", "left", "right"):
            facing = self.take().text
         if self.peek().text in TRANSITIONS:
            trans = self.take().text
         nowait = self.peek().kind == "name" and self.peek().text == WARP_NOWAIT_WORD
         if nowait:
            self.take()   # I4: the warp starts and the script goes on
         self.expect(";")
         return Node("warp", t, map=m.text, x=x, y=y, facing=facing, transition=trans, nowait=nowait)
      if self.accept("place"):
         obj = self.object_ref()
         x = self.operand()
         y = self.operand()
         facing = self.take().text if self.peek().text in ("down", "up", "left", "right") else None
         self.expect(";")
         return Node("place", t, obj=obj, x=x, y=y, facing=facing)
      if self.accept("scroll"):
         d = self.direction(("down", "up", "left", "right", "back"))
         cells = Node("num", t, value=0) if d == "back" else self.operand()
         speed = DEFAULT_SCROLL_SPEED
         if self.peek().text == "speed":
            self.take()
            speed, _ = self.number()
         self.expect(";")
         return Node("scroll", t, dir=d, cells=cells, speed=speed)
      if self.accept("tint"):
         if self.peek().text == "off":
            self.take()
            rgb, strength = (0, 0, 0), 0
         else:
            rgb = tuple(self.number()[0] for _ in range(RGB_CHANNELS))
            strength, _ = self.number()
         frames = self.number()[0] if self.peek().kind == "number" else 0
         self.expect(";")
         return Node("tint", t, rgb=rgb, strength=strength, frames=frames)
      if self.accept("flash"):
         rgb = tuple(self.number()[0] for _ in range(RGB_CHANNELS))
         frames, _ = self.number()
         self.expect(";")
         return Node("flash", t, rgb=rgb, frames=frames)
      if self.accept("shake"):
         power, _ = self.number()
         frames, _ = self.number()
         self.expect(";")
         return Node("shake", t, power=power, frames=frames)
      if self.accept("timer"):
         what = self.direction(("start", "stop"))
         secs = self.operand() if what == "start" else None
         self.expect(";")
         return Node("timer", t, start=what == "start", seconds=secs)
      if self.accept("encounters"):
         on = self.direction(("on", "off"))
         self.expect(";")
         return Node("encounters", t, on=on == "on")
      # G2: the bag statements are read by their first word only where the next token fits (no keywords: older
      # scripts may use the names)
      if self.peek().kind == "name" and self.peek().text in ITEM_STATEMENTS and self.peek(1).text == "(":
         kind = self.take().text
         self.expect("(")
         item = Node("item_name", self.peek(), name=self.take().text) if self.peek().kind == "string" \
            else self.operand()
         count = Node("num", t, value=1)
         if ITEM_STATEMENTS[kind]["counted"] and self.accept(","):
            count = self.operand()
         self.expect(")")
         result = None
         if self.accept("->"):
            result = self.var_ref().text
         elif ITEM_STATEMENTS[kind]["result"]:
            raise self.error(f"{kind} needs -> var(v)", t)
         self.expect(";")
         return Node("item_op", t, kind=kind, item=item, count=count, result=result)
      # G3: the box statements, read by their first word as the bag's are
      if self.peek().kind == "name" and self.peek().text in BOX_STATEMENTS and self.peek(1).text == "(":
         kind = self.take().text
         self.expect("(")
         at = self.operand()
         self.expect(")")
         result = self.var_ref().text if self.accept("->") else None
         self.expect(";")
         return Node("box_op", t, kind=kind, at=at, result=result)
      if self.at(FIELD_EFFECT_WORD) and self.peek(1).text in FIELD_EFFECT_KINDS:
         # F1: field_effect start NAME <amount> [-> var(ok)]; field_effect active|remaining NAME -> var(v);
         self.take()
         kind = self.take().text
         name = self.name("a field effect name").text
         amount = self.operand() if kind == "start" else None
         result = self.var_ref().text if self.accept("->") else None
         if result is None and kind != "start":
            raise self.error(f"field_effect {kind} needs -> var(v)", t)
         self.expect(";")
         return Node("field_effect", t, kind=kind, name=name, amount=amount, result=result)
      f2 = self.f2_statement()
      if f2 is not None:
         return f2
      if self.at("transform") and self.peek(1).text == "(":
         # G4: transform(<member>) -> var(v);
         self.take()
         self.expect("(")
         member = self.operand()
         self.expect(")")
         if not self.accept("->"):
            raise self.error("transform needs -> var(v)", t)
         n = self.var_ref()
         self.expect(";")
         return Node("transform", t, member=member, name=n.text)
      if self.peek().kind == "name" and self.peek().text in BOX_QUERIES and self.peek(1).text == "->":
         kind = self.take().text
         self.expect("->")
         n = self.var_ref()
         self.expect(";")
         return Node("box_query", t, kind=kind, name=n.text)
      if self.peek().kind == "name" and self.peek().text in ("open_bag", "time_of_day") and self.peek(1).text == "->":
         kind = self.take().text
         self.expect("->")
         n = self.var_ref()
         self.expect(";")
         return Node(kind, t, name=n.text)
      if self.at("shop") and self.peek(1).kind == "string":
         self.take()
         shop = self.take().text
         if not self.at("money"):
            raise self.error("shop \"Name\" money var(v); names the variable it is paid from", t)
         self.take()
         n = self.var_ref()
         self.expect(";")
         return Node("shop", t, shop=shop, name=n.text)
      if self.at("role_get") or self.at("role_set"):
         kind = self.take().text
         self.expect("(")
         slot = self.operand()
         self.expect(",")
         if self.peek().kind == "number":
            role, role_name = self.number()[0], None
         else:
            role, role_name = None, self.name("a role name or ID").text
         rest = []
         while self.accept(","):
            rest.append(self.operand())
         self.expect(")")
         most = ROLE_GET_TAIL if kind == "role_get" else ROLE_SET_TAIL
         if len(rest) > most or (kind == "role_set" and not rest):
            raise self.error(f"{kind} takes (slot, role, element)" if kind == "role_get"
                             else "role_set takes (slot, role, [element,] value)", t)
         element = rest[0] if len(rest) == most else Node("num", t, value=0)
         if kind == "role_get":
            self.expect("->")
            n = self.var_ref()
            self.expect(";")
            return Node("role_get", t, slot=slot, role=role, role_name=role_name, element=element, name=n.text)
         self.expect(";")
         return Node("role_set", t, slot=slot, role=role, role_name=role_name, element=element, value=rest[-1])
      if self.accept("vehicle"):
         if self.peek().kind == "number":
            index, _ = self.number()
            self.expect(";")
            return Node("vehicle", t, name=None, index=index)
         n = self.name("a vehicle class name or number")
         self.expect(";")
         return Node("vehicle", t, name=n.text, index=None)
      if self.at("when"):
         neg, games, glitch = self.when_head()
         body = self.block()
         other = self.block() if self.accept("else") else None
         return Node("when", t, negate=neg, games=games, glitch=glitch, body=body, other=other)
      i4 = self.i4_statement()
      if i4 is not None:
         return i4
      raise self.error(f"expected a statement, found {t.text or 'end of file'!r}")

   def i4_statement(self) -> Node | None:
      """!Parses an I4 statement (D-29: what imported event scripts need) when one starts here. Their first words
      are no keywords: a word starts one only when the next token is no ':' (a label) or '('.
      @return its node, or None (no I4 statement)
      """
      t = self.peek()
      if t.kind != "name" or t.text not in I4_WORDS or self.peek(1).text in (":", "("):
         return None
      w = self.take().text
      if w in ("lock", "release"):
         every = self.peek().kind == "name" and self.peek().text == "all"
         if every:
            self.take()
         self.expect(";")
         return Node(w, t, all=every)
      if w in ("wait_msg", "wait_state"):
         self.expect(";")
         return Node(w, t)
      if w == "remove":
         obj = self.object_ref()
         self.expect(";")
         return Node("remove", t, obj=obj)
      if w == "visible":
         obj = self.object_ref()
         on = self.direction(("on", "off"))
         self.expect(";")
         return Node("visible", t, obj=obj, on=on == "on")
      if w == "set_template":
         obj = self.object_ref()
         kind = self.direction(TEMPLATE_KINDS)
         a = b = None
         if kind == "pos":
            a, b = self.operand(), self.operand()
         elif kind == "movement":
            a = self.operand()
         self.expect(";")
         return Node("set_template", t, obj=obj, kind=kind, a=a, b=b)
      if w == "door":
         kind = self.direction(DOOR_KINDS)
         x = y = None
         if kind != "wait":
            x, y = self.operand(), self.operand()
         self.expect(";")
         return Node("door", t, kind=kind, x=x, y=y)
      if w == "weather":
         kind = self.direction(WEATHER_KINDS)
         value = self.operand() if kind == "set" else None
         self.expect(";")
         return Node("weather", t, kind=kind, value=value)
      if w == "fade":
         kind = self.direction(FADE_KINDS)
         speed = DEFAULT_FADE_SPEED
         swap = False
         while self.peek().text in ("speed", "swap"):
            if self.take().text == "speed":
               speed, _ = self.number()
            else:
               swap = True
         self.expect(";")
         return Node("fade", t, kind=kind, speed=speed, swap=swap)
      if w == "set_metatile":
         x, y, mid, solid = self.operand(), self.operand(), self.operand(), self.operand()
         self.expect(";")
         return Node("set_metatile", t, x=x, y=y, id=mid, solid=solid)
      if w == "respawn":
         value = self.operand()
         self.expect(";")
         return Node("respawn", t, value=value)
      if w == "override":   # DS1: a cell override set of the world
         kind = self.direction(OVERRIDE_KINDS)
         name = self.name("an override set's name")
         self.expect(";")
         return Node("override", t, kind=kind, set=name.text)
      if w == "set_warp":
         kind = self.direction(SET_WARP_KINDS)
         m = self.name("a map name")
         x, y = self.operand(), self.operand()
         self.expect(";")
         return Node("set_warp", t, kind=kind, map=m.text, x=x, y=y)
      if w == "sound":
         kind = self.direction(list(SOUND_KINDS))
         args = [self.operand() for _ in range(SOUND_KINDS[kind]) if not self.at(";")]
         self.expect(";")
         return Node("sound", t, kind=kind, args=args)
      if w == "wait_sound":
         kind = self.direction(SOUND_WAITS)
         self.expect(";")
         return Node("wait_sound", t, kind=kind)
      # mart NAME [kind];
      n = self.name("a mart name")
      kind = self.number()[0] if self.peek().kind == "number" else 0
      self.expect(";")
      return Node("mart_open", t, name=n.text, kind=kind)


# --- code generation -------------------------------------------------------------

class Emitter:
   """!Bytecode of one script, with labels and fixups."""

   def __init__(self) -> None:
      """!Creates an empty script."""
      ## Bytes.
      self.code = bytearray()
      ## Label name -> offset.
      self.labels = {}
      ## (position of a 16-bit operand, label name, node) to patch.
      self.fixups = []
      ## Counter for generated labels.
      self.serial = 0
      ## I4: positions of goto targets (a target naming another script makes the goto a Jump).
      self.goto_sites = set()

   def new_label(self) -> str:
      """!A fresh internal label name.
      @return the name
      """
      self.serial += 1
      return f".L{self.serial}"

   def place(self, label: str) -> None:
      """!Places a label at the current position.
      @param label  its name
      @return None
      """
      self.labels[label] = len(self.code)

   def op(self, name: str) -> None:
      """!Emits an opcode.
      @param name  opcode name
      @return None
      """
      self.code.append(OP[name])

   def u8(self, v: int) -> None:
      """!Emits a byte.
      @param v  0-255
      @return None
      """
      self.code.append(v & U8_MASK)

   def u16(self, v: int) -> None:
      """!Emits a 16-bit value.
      @param v  0-65535
      @return None
      """
      self.code += struct.pack("<H", v & U16_MASK)

   def u32(self, v: int) -> None:
      """!Emits a 32-bit value.
      @param v  the value (two's complement for negatives)
      @return None
      """
      self.code += struct.pack("<I", v & U32_MASK)

   def target(self, label: str, node: Node | None = None) -> None:
      """!Emits a 16-bit branch target, patched later.
      @param label  label name
      @param node   node for the error position
      @return None
      """
      self.fixups.append((len(self.code), label, node))
      self.u16(0)


class Compiler:
   """!Compiles a manifest and its sources; collects diagnostics."""

   def __init__(self, manifest: JsonDict, base: str | PathLike[str] | None = None,
                sources: Mapping[str, str] | None = None, asset_keys: Mapping[int, str | None] | None = None,
                map_names: Collection[str] | None = None, *,
                codepage: text_encoder.CodePage = text_encoder.DEFAULT) -> None:
      """!Prepares a compile.
      @param manifest    parsed capsule.scripts/1 manifest
      @param base        directory the manifest's paths are relative to
      @param sources     {path: text} to use instead of reading files (an editor's buffers), or None
      @param asset_keys  {key: kind} of the game's assets for text:key checks, or None (no check)
      @param map_names   set of map names for map_scripts checks, or None (no check)
      @param codepage    code page of the inline texts (the game's language)
      """
      ## The manifest.
      self.manifest = manifest
      ## Code page of the inline texts.
      self.codepage = codepage
      ## Path base.
      self.base = Path(base) if base else Path(".")
      ## Overriding source texts.
      self.overrides = sources or {}
      ## Known asset keys (None: unchecked).
      self.asset_keys = asset_keys
      ## Known map names (None: unchecked).
      self.map_names = map_names
      ## The world's vehicle class names in class order (None: no world; `vehicle` takes numbers only).
      self.vehicle_names = None
      ## The game's record layout (the manifest's "battle"; load_record_layout()), or None: registry roles only.
      self.record_layout = None
      ## The battle manifest's (item names, shop names) for the bag statements (G2), or None without a battle.
      self.inventory_names = None
      ## The battle manifest's timed field effects (F1; load_field_effects()): empty without a battle.
      self.field_effects = []
      ## Where their implicit declarations come from (diagnostics).
      self.field_effect_file = ""
      ## The battle manifest's field actions (F2; load_field_actions()): empty without a battle.
      self.field_actions = []
      ## The battle manifest's combination methods (B1; load_combine_methods()): empty without a battle.
      self.combine_methods = []
      ## The world's F2 data (load_world_field()), or None without a world.
      self.world_field = None
      ## The battle manifest's step effects (F3; load_step_effects()): empty without a battle.
      self.step_effects = []
      ## The battle manifest's eggs and nursery (B2; load_eggs()).
      self.eggs = {"eggs": False, "nursery": False, "hatch_script": None}
      ## Where their implicit declarations come from (diagnostics).
      self.step_effect_file = ""
      ## Where the F2 implicit declarations come from (diagnostics).
      self.field_action_file = ""
      ## The script being compiled uses self switches (set while compiling it).
      self.uses_self = False
      ## ("flag" | "var", name) the world's map events and objects use (no "never used" warning).
      self.world_uses = set()
      ## Diagnostics.
      self.diags = []
      ## Files read (for the dependency file).
      self.deps = []
      ## The recorded save shapes (C6; tools/save_shape.py load_history()).
      self.save_history = []
      ## Build variant.
      self.game = "base"
      ## Known variants (None: any).
      self.games = None
      ## Stack depth the build gives the VM.
      self.stack_depth = DEFAULT_STACK_DEPTH
      ## Flag name -> declaration node.
      self.flags = {}
      ## Variable name -> declaration node.
      self.vars = {}
      ## Constant name -> declaration node.
      self.consts = {}
      ## Native name -> declaration node.
      self.natives = {}
      ## Script name -> declaration node.
      self.scripts = {}
      ## Map name -> map_scripts node.
      self.maps = {}
      ## Declaration order of names.
      self.order = {"flag": [], "var": [], "script": [], "native": [], "map": []}
      ## Where each name was declared.
      self.where = {}
      ## Upper-cased name -> declaration (the IDs header upper-cases names).
      self.upper_names = {}
      ## Names referenced (for unused warnings).
      self.used = set()
      ## Compiled results.
      self.result = None
      ## The manifest's field placeholders (read_manifest()), or None before.
      self.field = None
      ## I4: the first local ID of the game's numbering (the manifest's object_base).
      self.object_base = 0
      ## C10: capsule::script::Bank::vm_flags (the manifest's "vm").
      self.vm_flags = 0
      ## I4: warn about unused flags and variables (the manifest's warn_unused).
      self.warn_unused = True
      ## I4: glitch id -> on (the manifest's glitches), for `when [not] glitch("id")`.
      self.glitch_switches = {}
      ## I4: movement name -> its declaration (named move routes).
      self.movements = {}
      ## I4: mart name -> its declaration.
      self.marts = {}
      ## I4: game route step names, in declaration order (their IDs).
      self.route_steps = []
      ## I4: script -> the scripts it jumps to (goto naming a script).
      self.jumps = {}
      ## Bank::placeholders entries: (kind, param, index, text offset), declaration order.
      self.placeholder_table = []
      ## Bytes of Bank::names.
      self.name_bytes = 0

   # Diagnostics.

   def err(self, msg: str, node: Node | None = None, file: str | None = None) -> None:
      """!Records an error.
      @param msg   text
      @param node  node for the position, or None
      @param file  source file (default: the node's)
      @return None
      """
      self.diags.append(Diagnostic("error", msg, file or (node or {}).get("file", ""),
                                   (node or {}).get("line", 0), (node or {}).get("col", 0)))

   def warn(self, msg: str, node: Node | None = None) -> None:
      """!Records a warning.
      @param msg   text
      @param node  node for the position, or None
      @return None
      """
      self.diags.append(Diagnostic("warning", msg, (node or {}).get("file", ""),
                                   (node or {}).get("line", 0), (node or {}).get("col", 0)))

   def errors(self) -> list[Diagnostic]:
      """!The errors recorded.
      @return list of Diagnostic
      """
      return [d for d in self.diags if d.severity == "error"]

   # Manifest and sources.

   def read_manifest(self) -> list[str]:
      """!Checks the manifest and returns its source paths.
      @return list of source paths as written
      """
      m = {k: v for k, v in self.manifest.items() if k != "editor"}
      allowed = {"schema", "sources", "game", "games", "stack_depth", "assets", "world", "placeholders", "terms",
                 "battle", save_shape.HISTORY_KEY, "object_base", "warn_unused", "glitches", "vm"}
      for k in m:
         if k not in allowed:
            raise ScriptError(f"unknown manifest key {k!r}")
      if m.get("schema") != "capsule.scripts/1":
         raise ScriptError("schema must be 'capsule.scripts/1'")
      sources = m.get("sources", [])
      if not isinstance(sources, list) or not all(isinstance(s, str) for s in sources):
         raise ScriptError("sources must be a list of file paths")
      self.game = m.get("game", "base")
      if m.get("games") is not None:
         self.games = list(m["games"])
         if self.game not in self.games:
            raise ScriptError(f"game {self.game!r} is not in games")
      depth = m.get("stack_depth", DEFAULT_STACK_DEPTH)
      if not isinstance(depth, int) or not 1 <= depth <= MAX_STACK:
         raise ScriptError(f"stack_depth must be 1-{MAX_STACK}")
      self.stack_depth = depth
      # I4: local IDs counted from object_base (an imported game's numbering), the unused warnings, the glitch switches
      base = m.get("object_base", 0)
      if isinstance(base, bool) or not isinstance(base, int) or not 0 <= base <= MAX_OBJECT_BASE:
         raise ScriptError(f"object_base must be 0-{MAX_OBJECT_BASE}")
      self.object_base = base
      self.vm_flags = vm_flags(m.get("vm", {}))
      if not isinstance(m.get("warn_unused", True), bool):
         raise ScriptError("warn_unused must be true or false")
      self.warn_unused = m.get("warn_unused", True)
      glitches = m.get("glitches", {})
      if not isinstance(glitches, dict) or not all(isinstance(k, str) and isinstance(v, bool)
                                                   for k, v in glitches.items()):
         raise ScriptError("glitches must map each glitch id to true (on) or false (off)")
      self.glitch_switches = dict(glitches)
      if m.get("battle") is not None:
         if not isinstance(m["battle"], str):
            raise ScriptError("battle must name the game's battle manifest")
         self.record_layout = load_record_layout(self.base / m["battle"], self.deps)
         self.inventory_names = load_inventory_names(self.base / m["battle"])
         self.field_effects = load_field_effects(self.base / m["battle"])
         self.field_effect_file = f"{m['battle']}: field_effects"
         self.field_actions = load_field_actions(self.base / m["battle"])
         self.combine_methods = load_combine_methods(self.base / m["battle"])
         self.step_effects = load_step_effects(self.base / m["battle"])
         self.step_effect_file = f"{m['battle']}: step_effects"
         self.eggs = load_eggs(self.base / m["battle"])
      self.field_action_file = f"{m.get('battle') or m.get('world')}: field_actions / flight_points"
      self.field = FieldPlaceholders(m, self.codepage, self.record_layout)
      # C6: the save history (recorded save shapes; tools/save_shape.py), when the file exists
      if m.get(save_shape.HISTORY_KEY) is not None and not isinstance(m[save_shape.HISTORY_KEY], str):
         raise ScriptError(f"{save_shape.HISTORY_KEY} must name the save history file")
      history = self.base / str(m.get(save_shape.HISTORY_KEY, save_shape.DEFAULT_HISTORY))
      try:
         self.save_history = save_shape.load_history(history)
      except save_shape.ShapeError as e:
         raise ScriptError(str(e)) from e
      if history.exists():
         self.deps.append(str(history))
      return sources

   def parse_all(self, paths: Iterable[str]) -> list[tuple[str, list[Node]]]:
      """!Parses every source; parse errors become diagnostics.
      @param paths  source paths from the manifest
      @return list of (file, [declaration nodes])
      """
      out = []
      for rel in paths:
         text = self.overrides.get(rel)
         if text is None:
            p = self.base / rel
            try:
               text = p.read_text(encoding="utf-8")
            except OSError as e:
               self.err(f"cannot read {rel}: {e.strerror}", file=rel)
               continue
            except UnicodeDecodeError as e:
               self.err(f"{rel} is not UTF-8 text (byte 0x{e.object[e.start]:02X} at offset {e.start})",
                        file=rel)
               continue
            self.deps.append(str(p))
         try:
            items = Parser(text, rel).file_items()
         except ScriptError as e:
            self.diags.append(Diagnostic("error", e.message, e.file, e.line, e.col))
            continue
         stamp(items, rel)
         out.append((rel, items))
      return out

   def game_selected(self, node: Node) -> bool:
      """!Whether a when-block is compiled in this build (I4: a glitch's when-block by the manifest's glitch switch).
      @param node  the when node
      @return true when its body is used (else its else-block, when it has one)
      """
      if node.get("glitch") is not None:
         gid = node["glitch"]
         if gid not in self.glitch_switches:
            self.err(f"unknown glitch {gid!r} (the manifest's glitches list each glitch the sources name)", node)
            return not node["negate"]
         return self.glitch_switches[gid] != node["negate"]
      known = self.games
      for g in node["games"]:
         if known is not None and g not in known:
            self.err(f"unknown game {g!r} (games: {', '.join(known)})", node)
      inside = self.game in node["games"]
      return inside != node["negate"]

   def when_body(self, node: Node) -> list[Node]:
      """!The part of a when-block this build uses.
      @param node  the when node
      @return its body, its else-block, or nothing
      """
      if self.game_selected(node):
         return node["body"]
      return node.get("other") or []

   def declare(self, items: Iterable[Node]) -> None:
      """!Collects declarations (when-blocks resolved).
      @param items  declaration nodes
      @return None
      """
      for d in items:
         k = d["k"]
         if k == "when":
            self.declare(self.when_body(d))
            continue
         name = d["name"]
         if k == "map_scripts":   # map names live in their own space
            if name in self.maps:
               self.err(f"map_scripts {name!r} appears twice", d)
               continue
            if self.map_names is not None and name not in self.map_names:
               self.err(f"map {name!r} is not in the world", d)
            self.maps[name] = d
            self.order["map"].append(name)
            continue
         if name in self.where:
            other = self.where[name]
            self.err(f"{name!r} is already declared at {other['file']}:{other['line']}", d)
            continue
         folded = self.upper_names.get(name.upper())
         if folded is not None:
            self.err(f"{name!r} differs from {folded['name']!r} ({folded['file']}:{folded['line']}) only in "
                     f"case; the IDs header names both {name.upper()}", d)
            continue
         self.upper_names[name.upper()] = d
         self.where[name] = d
         if k == "flag":
            self.flags[name] = d
            self.order["flag"].append(name)
         elif k == "var":
            width, lo, hi = TYPES[d["type"]]
            if d["range"]:
               a, b = d["range"]
               if not lo <= a <= b <= hi:
                  self.err(f"range {a}..{b} does not fit {d['type']} ({lo}..{hi})", d)
               lo, hi = a, b
            d["width"], d["lo"], d["hi"] = width, lo, hi
            self.vars[name] = d
            self.order["var"].append(name)
         elif k == "const":
            if not INT32_MIN <= d["value"] < INT32_LIMIT:
               self.err("constants must fit 32 bits", d)
            self.consts[name] = d
         elif k == "native":
            if len(d["params"]) > MAX_NATIVE_ARGS:
               self.err(f"a native takes at most {MAX_NATIVE_ARGS} arguments", d)
            self.natives[name] = d
            self.order["native"].append(name)
         elif k == "script":
            self.scripts[name] = d
            self.order["script"].append(name)
         elif k == "movement":
            self.movements[name] = d
         elif k == "mart":
            if len(d["items"]) > MAX_MART_ITEMS:
               self.err(f"a mart lists at most {MAX_MART_ITEMS} items", d)
            self.marts[name] = d
         elif k == "step":
            steps = self.route_steps   # a local alias (Doxygen 1.12 reads `self.x.append()` as a new member)
            if len(steps) == MAX_ROUTE_STEPS:
               self.err(f"at most {MAX_ROUTE_STEPS} game route steps", d)
            steps.append(name)

   # Operands.

   def value(self, node: Node) -> int | None:
      """!The constant value of an operand node, or None for var(...).
      @param node  operand node
      @return int or None
      """
      k = node["k"]
      if k == "num":
         v = node["value"]
         if not INT32_MIN <= v < INT32_LIMIT:
            self.err("values must fit 32 bits", node)
            return 0
         return v
      if k == "constname":
         c = self.consts.get(node["name"])
         if c is None:
            self.err(f"unknown constant {node['name']!r}", node)
            return 0
         return c["value"]
      if k == "ref":
         try:
            key = asset_key(node["ref"], "reference")
         except KeyError_ as e:
            self.err(str(e), node)
            return 0
         # The unsigned key as the signed 32-bit value the operand stores.
         return key - U32_RANGE if key >= INT32_LIMIT else key
      if k == "scriptref":
         if node["name"] not in self.scripts:
            self.err(f"unknown script {node['name']!r}", node)
            return 0
         self.used.add(("script", node["name"]))
         return self.script_ids.get(node["name"], 0)
      return None

   def check_object(self, v: int, obj: Node, node: Node, last: bool = False) -> None:
      """!Checks a constant object operand (I4: local IDs count from the manifest's object_base; the camera; the
      object last given a route where it may stand).
      @param v     its value
      @param obj   its node
      @param node  statement node for errors
      @param last  `last` is allowed (wait_moves)
      @return None
      """
      word = obj.get("obj")
      if word == "last" and not last:
         self.err("last names the object last given a route only in wait_moves", node)
      first = self.object_base
      if word is None and not first <= v <= first + MAX_OBJECT:
         self.err(f"object {v} is not a local ID ({first}-{first + MAX_OBJECT}); use self or player for those", node)

   def var_id(self, name: str, node: Node) -> int:
      """!The ID of a variable.
      @param name  its name
      @param node  node for errors
      @return its ID (0 when unknown)
      """
      if name not in self.vars:
         self.err(f"unknown variable {name!r}", node)
         return 0
      self.used.add(("var", name))
      return self.var_ids[name]

   def flag_id(self, name: str, node: Node) -> int:
      """!The ID of a flag.
      @param name  its name
      @param node  node for errors
      @return its ID (0 when unknown)
      """
      if name not in self.flags:
         self.err(f"unknown flag {name!r}", node)
         return 0
      self.used.add(("flag", name))
      return self.flag_ids[name]

   def emit_operand(self, e: Emitter, node: Node) -> None:
      """!Emits an operand.
      @param e     emitter
      @param node  operand node
      @return None
      """
      if node["k"] == "query":
         e.u8(OPND_QUERY)
         e.u8(list(QUERIES).index(node["query"]))
         obj = node.get("obj")
         if obj is None:
            e.u8(0)
         else:
            v = self.value(obj)
            if v is None:
               self.err("a query names its object with self, player or a constant local ID", node)
               v = 0
            self.check_object(v, obj, node)
            e.u8(v)
         return
      v = self.value(node)
      # Constants take the smallest encoding that holds them.
      if v is None:
         e.u8(OPND_VAR)
         e.u16(self.var_id(node["name"], node))
      elif 0 <= v <= U8_MAX:
         e.u8(OPND_CONST8)
         e.u8(v)
      elif INT16_MIN <= v <= INT16_MAX:
         e.u8(OPND_CONST16)
         e.u16(v)
      else:
         e.u8(OPND_CONST32)
         e.u32(v)

   def pool_text(self, codes: list[int]) -> int:
      """!Adds encoded text to the inline text pool (identical texts stored once).
      @param codes  codes ending with CODE_END
      @return its offset in text_pool
      """
      key = bytes(codes)
      if key not in self.text_offsets:
         self.text_offsets[key] = len(self.text_pool)
         self.text_pool += codes
      return self.text_offsets[key]

   def resolve_placeholders(self) -> None:
      """!Builds the bank's placeholder table: variable IDs, term texts (in the text pool), name slots.
      @return None
      """
      self.placeholder_table = []
      self.name_bytes = 0
      if self.field is None:
         return
      for name, (kind, value, extra) in self.field.bindings.items():
         where = f"placeholder {{{name}}}"
         if kind == "name":
            self.placeholder_table.append((PLACEHOLDER_KINDS.index(kind), value, 0, self.name_bytes))
            self.name_bytes += NAME_LENGTH_BYTES + value
         elif kind == "var":
            if value not in self.var_ids:
               self.err(f"{where}: unknown variable {value!r}")
               self.placeholder_table.append((PLACEHOLDER_KINDS.index(kind), 0, 0, 0))   # keeps the indices
               continue
            self.used.add(("var", value))
            self.placeholder_table.append((PLACEHOLDER_KINDS.index(kind), 0, self.var_ids[value], 0))
         elif kind == "term":
            codes = self.field.term_codes(where, value, extra.get("form", TERM_ONE))
            self.placeholder_table.append((PLACEHOLDER_KINDS.index(kind), 0, 0, self.pool_text(codes)))
         else:
            # a record binding: the role's ID; Placeholder::text = its text codes (0: printed in decimal)
            self.placeholder_table.append((PLACEHOLDER_KINDS.index(kind), extra.get("slot", 0),
                                           self.field.roles[value][0], self.field.text_codes(value)))
      if self.name_bytes > U16_MASK:
         self.err("the name slots take more than 64 KB")

   def emit_text(self, e: Emitter, node: Node) -> None:
      """!Emits a text reference.
      @param e     emitter
      @param node  inline or textkey node
      @return None
      """
      if node["k"] == "inline":
         try:
            if self.field is not None and self.field.declared:
               codes = self.field.encode(node["text"], "text")
            else:
               codes = encode_text(node["text"], "text", self.codepage)
         except (AssetError, text_encoder.TextError) as ex:
            self.err(str(ex).replace("text: ", ""), node)
            codes = [CODE_END]
         e.u8(TEXT_INLINE)
         e.u32(self.pool_text(codes))
      else:
         try:
            k = asset_key(node["ref"], "text")
         except KeyError_ as ex:
            self.err(str(ex), node)
            k = 0
         if self.asset_keys is not None and self.asset_keys.get(k) != "text":
            self.err(f"{node['ref']} is not a text asset of the game", node)
         e.u8(TEXT_ASSET)
         e.u32(k)

   # Conditions.

   def deps_of(self, cond: Node, out: set[tuple[str, str]]) -> None:
      """!Collects the flags and variables a condition reads.
      @param cond  condition node
      @param out   set of (kind, name); extended
      @return None
      """
      k = cond["k"]
      if k in ("and", "or"):
         self.deps_of(cond["a"], out)
         self.deps_of(cond["b"], out)
      elif k == "not":
         self.deps_of(cond["a"], out)
      elif k == "flagc":
         out.add(("flag", cond["name"]))
      elif k == "selfc":
         out.add(("dynamic", "self switches"))
      elif k == "cmp":
         for side in (cond["a"], cond["b"]):
            if side["k"] == "varop":
               out.add(("var", side["name"]))
            elif side["k"] == "query":
               out.add(("dynamic", side["query"]))

   def jump(self, e: Emitter, cond: Node, label: str, when: bool) -> None:
      """!Emits a jump to @p label taken when @p cond is @p when.
      @param e      emitter
      @param cond   condition node
      @param label  target label
      @param when   jump when true (True) or false (False)
      @return None
      """
      k = cond["k"]
      if k == "not":
         self.jump(e, cond["a"], label, not when)
      elif k in ("and", "or"):
         # a && b is true when both are; a || b when either is.
         both = k == "and"
         if when == both:
            skip = e.new_label()
            self.jump(e, cond["a"], skip, not when)
            self.jump(e, cond["b"], label, when)
            e.place(skip)
         else:
            self.jump(e, cond["a"], label, when)
            self.jump(e, cond["b"], label, when)
      elif k == "flagc":
         e.op("IfFlagSet" if when else "IfFlagClear")
         e.u16(self.flag_id(cond["name"], cond))
         e.target(label, cond)
      elif k == "selfc":
         e.op("IfSelfSet" if when else "IfSelfClear")
         e.u8(SELF_LETTERS.index(cond["letter"]))
         e.target(label, cond)
         self.uses_self = True
      else:
         op = cond["op"] if when else NEGATE[cond["op"]]
         a, b = self.value(cond["a"]), self.value(cond["b"])
         if a is not None and b is not None:   # both constant: decided now
            holds = {"==": a == b, "!=": a != b, "<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b}[op]
            if holds:
               e.op("Goto")
               e.target(label, cond)
            return
         e.op("IfCompare")
         e.u8(COMPARES.index(op))
         self.emit_operand(e, cond["a"])
         self.emit_operand(e, cond["b"])
         e.target(label, cond)

   # Statements.

   def check_span(self, name: str, node: Node, lo: int, hi: int, what: str) -> None:
      """!Errors when values a statement can store are outside a variable's range,
      where clamping would merge different answers (P-18).
      @param name  variable name
      @param node  node for errors
      @param lo    lowest value stored
      @param hi    highest value stored
      @param what  what stores them, for the message
      @return None
      """
      v = self.vars.get(name)
      if v is not None and not (v["lo"] <= lo and hi <= v["hi"]):
         self.err(f"{what} stores {lo}..{hi}, outside {name}'s range {v['lo']}..{v['hi']}", node)

   def check_range(self, name: str, node: Node, value: int | None) -> None:
      """!Errors when a constant is outside a variable's declared range (P-18).
      @param name   variable name
      @param node   node for errors
      @param value  the constant, or None
      @return None
      """
      v = self.vars.get(name)
      if v is not None and value is not None and not v["lo"] <= value <= v["hi"]:
         self.err(f"{value} is outside {name}'s range {v['lo']}..{v['hi']}", node)

   def statements(self, e: Emitter, body: Iterable[Node], ctx: dict[str, Any]) -> bool:
      """!Emits a list of statements.
      @param e     emitter
      @param body  statement nodes
      @param ctx   per-script state: 'calls' (set), 'waits' (bool), 'loops' (break stack)
      @return true when control can fall off the end of the list
      """
      live = True
      for s in body:
         if not live and s["k"] != "label":
            self.warn("unreachable statement", s)
            live = True   # warn once per run
         live = self.statement(e, s, ctx)
      return live

   def statement(self, e: Emitter, s: Node, ctx: dict[str, Any]) -> bool:
      """!Emits one statement.
      @param e    emitter
      @param s    statement node
      @param ctx  per-script state (see statements())
      @return true when control continues after it
      """
      k = s["k"]
      if k == "label":
         if s["name"] in e.labels:
            self.err(f"label {s['name']!r} appears twice", s)
         e.place(s["name"])
         return True
      if k == "goto":
         e.op("Goto")
         e.goto_sites.add(len(e.code))   # I4: a goto naming a script becomes a Jump (compile_script())
         e.target(s["label"], s)
         return False
      if k == "call":
         if self.scripts.get(s["script"]) is None:
            self.err(f"unknown script {s['script']!r}", s)
         else:
            self.used.add(("script", s["script"]))
            # I4: a loose call adds no depth to the proof (a full stack makes it a jump at run time)
            ctx["loose" if s.get("loose") else "calls"].add(s["script"])
         e.op("CallLoose" if s.get("loose") else "Call")
         e.u16(self.script_ids.get(s["script"], 0))
         return True
      if k == "calltable":
         e.op("CallTable")
         self.emit_operand(e, s["sel"])
         if len(s["targets"]) > MAX_CALL_TABLE:
            self.err("a call table names at most 255 scripts", s)
         e.u8(len(s["targets"]))
         for t in s["targets"]:
            if self.scripts.get(t) is None:
               self.err(f"unknown script {t!r}", s)
            else:
               self.used.add(("script", t))
               ctx["calls"].add(t)
            e.u16(self.script_ids.get(t, 0))
         return True
      if k == "return":
         e.op("Return")
         return False
      if k == "end":
         e.op("End")
         return False
      if k == "if":
         other = e.new_label()
         done = e.new_label()
         self.jump(e, s["cond"], other, False)
         live = self.statements(e, s["then"], ctx)
         if s["other"] is not None:
            if live:
               e.op("Goto")
               e.target(done, s)
            e.place(other)
            live = self.statements(e, s["other"], ctx) or live
            e.place(done)
            return live
         e.place(other)
         e.place(done)
         return True
      if k == "while":
         top = e.new_label()
         done = e.new_label()
         e.place(top)
         self.jump(e, s["cond"], done, False)
         self.statements(e, s["body"], ctx)
         e.op("Goto")
         e.target(top, s)
         e.place(done)
         return True
      if k == "switch":
         done = e.new_label()
         labels = []
         seen = set()
         for c in s["cases"]:
            lab = e.new_label()
            labels.append(lab)
            for v in c["values"]:
               cv = self.value(v)
               if cv is None:
                  self.err("case values are constants", v)
                  continue
               if cv in seen:
                  self.err(f"case {cv} appears twice", v)
               seen.add(cv)
               self.jump(e, Node("cmp", Token("sym", "==", v["line"], v["col"]), op="==", a=s["value"],
                                 b=Node("num", Token("number", str(cv), v["line"], v["col"]), value=cv)),
                         lab, True)
         if s["default"] is not None:
            self.statements(e, s["default"], ctx)
         e.op("Goto")
         e.target(done, s)
         for c, lab in zip(s["cases"], labels):
            e.place(lab)
            if self.statements(e, c["body"], ctx):
               e.op("Goto")
               e.target(done, c)
         e.place(done)
         return True
      if k == "setflag":
         e.op("SetFlag" if s["on"] else "ClearFlag")
         e.u16(self.flag_id(s["name"], s))
         return True
      if k == "setself":
         e.op("SelfSwitch")
         e.u8(SELF_LETTERS.index(s["letter"]))
         e.u8(1 if s["on"] else 0)
         self.uses_self = True
         return True
      if k == "assign" and s["op"] in VAR_OPS:
         vid = self.var_id(s["name"], s)
         if s["op"] in ("/=", "%=") and self.value(s["value"]) == 0:
            self.err(f"{s['op']} 0 divides by zero", s)
         e.op("VarOp")
         e.u8(VAR_OPS.index(s["op"]))
         e.u16(vid)
         self.emit_operand(e, s["value"])
         return True
      if k == "assign":
         vid = self.var_id(s["name"], s)
         e.op({"=": "SetVar", "+=": "AddVar", "-=": "SubVar"}[s["op"]])
         e.u16(vid)
         if s["op"] == "=":
            self.check_range(s["name"], s, self.value(s["value"]))
         self.emit_operand(e, s["value"])
         return True
      if k == "random":
         vid = self.var_id(s["name"], s)
         bound = self.value(s["bound"])
         if bound is not None:
            if not 1 <= bound <= MAX_RANDOM_BOUND:
               self.err("random(n) takes n from 1 to 65536", s)
            else:
               self.check_span(s["name"], s, 0, bound - 1, f"random({bound})")
         e.op("Random")
         e.u16(vid)
         self.emit_operand(e, s["bound"])
         return True
      if k == "msg":
         kind = s.get("kind")
         if kind in ("npc", "sign"):
            # I4: lock (all for a sign), an NPC turns to the player
            e.op("Lock")
            e.u8(LOCK_ALL if kind == "sign" else LOCK_SELF)
            if kind == "npc":
               e.op("ObjectFace")
               self.emit_operand(e, Node("num", Token("number", str(OBJECT_SELF), s["line"], s["col"]),
                                         value=OBJECT_SELF))
               e.u8(FACE_PLAYER)
         if s.get("flags"):
            e.op("MessageEx")   # I4: no wait / instant / autoscroll
            e.u8(s["flags"])
            self.emit_text(e, s["text"])
         else:
            e.op("Message")
            self.emit_text(e, s["text"])
         if kind == "yesno":
            e.op("YesNo")
            e.u16(self.var_id(s["result"], s))
            self.check_span(s["result"], s, 0, 1, "msgbox yesno")
         elif s["box"]:
            e.op("WaitButton")
         if kind in ("npc", "sign", "autoclose"):
            e.op("Release")
            e.u8(LOCK_ALL if kind == "sign" else LOCK_SELF)
         if not s.get("flags", 0) & MSG_FLAGS["nowait"] or s["box"]:
            ctx["waits"] = True
         return True
      if k == "wait_button":
         e.op("WaitButton")
         ctx["waits"] = True
         return True
      if k == "close_msg":
         e.op("CloseMessage")
         return True
      if k == "yesno":
         e.op("YesNo")
         e.u16(self.var_id(s["name"], s))
         self.check_span(s["name"], s, 0, 1, "yesno")
         ctx["waits"] = True
         return True
      if k == "choice":
         n = len(s["options"])
         if n > MAX_CHOICES:
            self.err(f"a choice has at most {MAX_CHOICES} options", s)
         cancel = NO_CANCEL if s["cancel"] is None else s["cancel"]
         if s["cancel"] is not None and not 0 <= s["cancel"] < n:
            self.err(f"cancel must be an option index 0-{n - 1}", s)
         e.op("Choice")
         e.u16(self.var_id(s["name"], s))
         self.check_span(s["name"], s, 0, n - 1, f"a choice of {n}")
         e.u8(cancel)
         e.u8(n)
         for o in s["options"]:
            self.emit_text(e, o)
         ctx["waits"] = True
         return True
      if k == "delay":
         e.op("Delay")
         v = self.value(s["frames"])
         if v is not None and v < 0:
            self.err("delay frames cannot be negative", s)
         self.emit_operand(e, s["frames"])
         ctx["waits"] = True
         return True
      if k == "native":
         nat = self.natives.get(s["name"])
         if nat is None:
            self.err(f"unknown native {s['name']!r} (declare it with 'native {s['name']}(...);')", s)
            nid, nat = 0, {"params": s["args"], "returns": True, "waits": False}
         else:
            nid = self.native_ids[s["name"]]
            self.used.add(("native", s["name"]))
         if len(s["args"]) != len(nat["params"]):
            self.err(f"{s['name']} takes {len(nat['params'])} argument(s), not {len(s['args'])}", s)
         if s["result"] is not None and not nat["returns"]:
            self.err(f"{s['name']} returns no value", s)
         e.op("Native")
         e.u16(nid)
         e.u8(len(s["args"]))
         for a in s["args"]:
            self.emit_operand(e, a)
         e.u16(self.var_id(s["result"], s) if s["result"] is not None else NO_VAR)
         if nat["waits"]:
            ctx["waits"] = True
         return True
      if k in ("move", "face", "showobj"):
         obj = s["obj"]
         v = self.value(obj)
         if v is not None:
            self.check_object(v, obj, s)
         if k == "move":
            e.op("ObjectMoveChecked" if s.get("checked") else "ObjectMove")
            self.emit_operand(e, obj)
            e.u8(DIRECTIONS.index(s["dir"]))
            v = self.value(s["steps"])
            if v is not None and not 0 <= v <= MAX_MOVE_STEPS:
               self.err("move takes 0-65535 steps", s)
            self.emit_operand(e, s["steps"])
            if s.get("checked"):
               e.u16(self.var_id(s["result"], s) if s["result"] is not None else NO_VAR)
            ctx["waits"] = True
            return True
         if k == "face":
            if s["dir"] == obj.get("obj"):
               self.err(f"{s['dir']} cannot face itself", s)
            e.op("ObjectFace")
            self.emit_operand(e, obj)
            e.u8(DIRECTIONS.index(s["dir"]) if s["dir"] in DIRECTIONS else FACE_WORDS[s["dir"]])
            return True
         if obj.get("obj") == "player":
            self.err("the player cannot be hidden or shown (only map objects)", s)
         e.op("ObjectShow" if s["on"] else "ObjectHide")
         self.emit_operand(e, obj)
         return True
      r = self.eventing_statement(e, s, ctx)
      if r is not None:
         return r
      if k == "when":
         return self.statements(e, self.when_body(s), ctx)
      r = self.i4_emit(e, s, ctx)
      if r is not None:
         return r
      self.err(f"internal: unknown statement {k}", s)
      return True

   def eventing_statement(self, e: Emitter, s: Node, ctx: dict[str, Any]) -> bool | None:
      """!Emits the RPG Maker XP-style eventing statements (D-17).
      @param e    emitter
      @param s    statement node
      @param ctx  per-script state
      @return True / False as statement(), or None when @p s is not one of them
      """
      k = s["k"]
      if k == "route":
         obj = s["obj"]
         v = self.value(obj)
         if v is not None:
            self.check_object(v, obj, s)
         if s["repeat"] and obj.get("obj") == "player":
            self.err("the player cannot take a repeating route", s)

         def flag(name: str) -> int:
            """!A flag's ID for the route compiler.
            @param name  flag name
            @return its ID
            """
            return self.flag_id(name, s)

         def sprite(ref: str) -> int:
            """!A sprite's key for the route compiler.
            @param ref  sprite:NAME
            @return its key
            """
            try:
               return asset_key(ref, "route sprite")
            except KeyError_ as err:
               self.err(str(err), s)
               return 0
         steps = s["steps"]
         where = s
         if s.get("movement") is not None:
            # I4: a named movement's steps
            mv = self.movements.get(s["movement"])
            if mv is None:
               self.err(f"unknown movement {s['movement']!r}", s)
               steps = []
            else:
               steps = mv["steps"]
               where = mv
         try:
            code = rc.compile_route(steps, s["repeat"], s["skip"], flag, sprite, self.route_step_id)
         except rc.RouteError as err:
            self.err(f"route: {err}", where)
            code = bytes([0, 0])
         e.op("Route")
         self.emit_operand(e, obj)
         e.u8(ROUTE_NOWAIT if s["nowait"] else 0)
         e.u16(len(code))
         e.code += code
         if not s["nowait"]:
            ctx["waits"] = True
         return True
      if k == "wait_moves":
         if s.get("obj") is not None:
            # I4: one object's route (or the object last given one)
            v = self.value(s["obj"])
            if v is not None:
               self.check_object(v, s["obj"], s, last=True)
            e.op("WaitObject")
            self.emit_operand(e, s["obj"])
         else:
            e.op("WaitMoves")
         ctx["waits"] = True
         return True
      if k == "input_number":
         if not 1 <= s["digits"] <= MAX_INPUT_DIGITS:
            self.err("input_number takes 1-9 digits", s)
         vid = self.var_id(s["name"], s)
         # n digits store 0 .. 10^n - 1 (the digit count clamped as the VM clamps it).
         digits = max(1, min(MAX_INPUT_DIGITS, s["digits"]))
         self.check_span(s["name"], s, 0, DECIMAL_BASE ** digits - 1, f"{s['digits']} digits")
         e.op("InputNumber")
         e.u16(vid)
         e.u8(digits)
         ctx["waits"] = True
         return True
      if k == "wait_key":
         vid = self.var_id(s["name"], s)
         self.check_span(s["name"], s, 1, MAX_KEY_CODE, "a key code")
         e.op("WaitKey")
         e.u16(vid)
         ctx["waits"] = True
         return True
      if k == "msg_pos":
         e.op("MessagePos")
         e.u8(MSG_POSITIONS.index(s["pos"]))
         return True
      if k == "warp":
         known = self.map_names
         if known is not None and s["map"] not in known:
            self.err(f"warp: {s['map']!r} is not a map of the world", s)
         e.op("Warp")
         e.u32(asset_key(f"map:{s['map']}", "map"))
         self.emit_operand(e, s["x"])
         self.emit_operand(e, s["y"])
         e.u8((DIRECTIONS.index(s["facing"]) if s["facing"] else KEEP_FACING) | (WARP_NOWAIT if s.get("nowait")
                                                                                 else 0))
         e.u8(TRANSITION_DEFAULT if s["transition"] == "default" else TRANSITIONS.index(s["transition"]))
         if not s.get("nowait"):
            ctx["waits"] = True
         return True
      if k == "place":
         v = self.value(s["obj"])
         if v is not None:
            self.check_object(v, s["obj"], s)
         if s["obj"].get("obj") == "camera":
            self.err("place takes a map object or the player", s)
         e.op("Place")
         self.emit_operand(e, s["obj"])
         self.emit_operand(e, s["x"])
         self.emit_operand(e, s["y"])
         e.u8(DIRECTIONS.index(s["facing"]) if s["facing"] else KEEP_FACING)
         return True
      if k == "scroll":
         if not 1 <= s["speed"] <= MAX_SCROLL_SPEED:
            self.err("scroll speed is 1-4", s)
         e.op("Scroll")
         e.u8(SCROLL_BACK if s["dir"] == "back" else DIRECTIONS.index(s["dir"]))
         self.emit_operand(e, s["cells"])
         e.u8(max(1, min(MAX_SCROLL_SPEED, s["speed"])))
         ctx["waits"] = True
         return True
      if k in ("tint", "flash"):
         if any(not 0 <= c <= MAX_COLOR_CHANNEL for c in s["rgb"]):
            self.err(f"{k} colors are 0-31 (red, green, blue)", s)
         if k == "tint" and not 0 <= s["strength"] <= MAX_TINT_STRENGTH:
            self.err("tint strength is 0-16", s)
         if not 0 <= s["frames"] <= MAX_EFFECT_FRAMES or (k == "flash" and s["frames"] < 1):
            self.err(f"{k} frames are {'1' if k == 'flash' else '0'}-65535", s)
         e.op("Tint" if k == "tint" else "Flash")
         for c in s["rgb"]:
            e.u8(c & CHANNEL_MASK)
         if k == "tint":
            e.u8(max(0, min(MAX_TINT_STRENGTH, s["strength"])))   # 0-16, as checked above
         e.u16(s["frames"] & U16_MASK)
         return True
      if k == "shake":
         if not 1 <= s["power"] <= MAX_SHAKE_POWER or not 1 <= s["frames"] <= MAX_EFFECT_FRAMES:
            self.err("shake takes a power of 1-8 pixels and 1-65535 frames", s)
         e.op("Shake")
         e.u8(s["power"] & U8_MASK)
         e.u16(s["frames"] & U16_MASK)
         return True
      if k == "timer":
         if s["start"]:
            v = self.value(s["seconds"])
            if v is not None and not 1 <= v <= MAX_TIMER_SECONDS:
               self.err("timer start takes 1-5999 seconds", s)
            e.op("TimerStart")
            self.emit_operand(e, s["seconds"])
         else:
            e.op("TimerStop")
         return True
      if k == "encounters":
         e.op("Encounters")
         e.u8(1 if s["on"] else 0)
         return True
      if k == "item_op":
         opcode = ITEM_STATEMENTS[s["kind"]]["op"]
         e.op(opcode)
         e.u16(self.var_id(s["result"], s) if s["result"] is not None else NO_VAR)
         self.emit_item(e, s["item"], s)
         if opcode != "ItemCount":
            n = self.value(s["count"])
            if n is not None and not 1 <= n <= MAX_ITEM_COUNT:
               self.err(f"{s['kind']}: {n} items (1-{MAX_ITEM_COUNT})", s)
            self.emit_operand(e, s["count"])
         return True
      if k == "box_op":
         e.op(BOX_STATEMENTS[s["kind"]])
         e.u16(self.var_id(s["result"], s) if s["result"] is not None else NO_VAR)
         self.emit_operand(e, s["at"])
         return True
      if k == "field_effect":
         names = [fx["name"] for fx in self.field_effects]
         if s["name"] not in names:
            self.err(f"field_effect: {s['name']!r} is not a field effect of the battle manifest's structure "
                     f"({', '.join(names) or 'none: the manifest needs its battle'})", s)
            index = 0
         else:
            index = names.index(s["name"])
         e.op("FieldEffect")
         e.u8(FIELD_EFFECT_KINDS.index(s["kind"]))
         e.u8(index)
         if s["kind"] == "start":
            amount = self.value(s["amount"])
            # C8: the effect compiler's check too (its "field-effect-amount" message): the variable is a u16
            from battlec.field import MAX_AMOUNT
            if amount is not None and not 1 <= amount <= MAX_AMOUNT:
               self.err(f"field_effect start: the amount is {amount}; it is 1 to {MAX_AMOUNT}", s)
            self.emit_operand(e, s["amount"])
         e.u16(self.var_id(s["result"], s) if s["result"] is not None else NO_VAR)
         return True
      handled = self.f2_emit(e, s, ctx)
      if handled is not None:
         return handled
      if k == "transform":
         e.op("Transform")
         e.u16(self.var_id(s["name"], s))
         self.emit_operand(e, s["member"])
         ctx["waits"] = True
         return True
      if k == "box_query":
         e.op(BOX_QUERIES[s["kind"]])
         e.u16(self.var_id(s["name"], s))
         if s["kind"] == "open_boxes":
            ctx["waits"] = True
         return True
      if k in ("open_bag", "time_of_day"):
         e.op("BagOpen" if k == "open_bag" else "TimeOfDay")
         e.u16(self.var_id(s["name"], s))
         if k == "open_bag":
            ctx["waits"] = True
         return True
      if k == "shop":
         names = self.inventory_names[1] if self.inventory_names is not None else None
         if names is None:
            self.err("shop: shop names need the manifest's battle", s)
            index = 0
         elif s["shop"] not in names:
            self.err(f"shop: {s['shop']!r} is not a shop of the battle manifest ({', '.join(names) or 'none'})", s)
            index = 0
         else:
            index = names.index(s["shop"])
         e.op("Shop")
         e.u8(index)
         e.u16(self.var_id(s["name"], s))
         ctx["waits"] = True
         return True
      if k in ("role_get", "role_set"):
         rid, shape = self.role_ref(s)
         slot = self.value(s["slot"])
         if slot is not None and not 0 <= slot < self.field.party_size:
            self.err(f"{k}: party slot {slot} does not exist (0-{self.field.party_size - 1})", s)
         element = self.value(s["element"])
         count = shape[1] if shape is not None else MAX_ROLE_ELEMENT + 1
         if element is not None and not 0 <= element < count:
            self.err(f"{k}: element {element} does not exist (0-{count - 1})", s)
         hi = min((1 << shape[0]) - 1, MAX_VAR_VALUE) if shape is not None else MAX_VAR_VALUE
         if k == "role_get":
            vid = self.var_id(s["name"], s)
            self.check_span(s["name"], s, 0, hi, f"role_get of a {shape[0]}-bit role" if shape else "role_get")
            e.op("RoleGet")
            e.u16(vid)
            self.emit_operand(e, s["slot"])
            e.u8(rid)
            self.emit_operand(e, s["element"])
            return True
         value = self.value(s["value"])
         if value is not None and not 0 <= value <= hi:
            self.err(f"role_set: {value} does not fit the role (0-{hi})", s)
         e.op("RoleSet")
         self.emit_operand(e, s["slot"])
         e.u8(rid)
         self.emit_operand(e, s["element"])
         self.emit_operand(e, s["value"])
         return True
      if k == "vehicle":
         known = self.vehicle_names
         index = s["index"]
         if s["name"] is not None:
            if known is None:
               self.err(f"vehicle {s['name']}: class names need the manifest's world (or give a class number)", s)
               index = 0
            elif s["name"] not in known:
               self.err(f"vehicle: {s['name']!r} is not a vehicle class of the world ({', '.join(known)})", s)
               index = 0
            else:
               index = known.index(s["name"])
         limit = len(known) if known is not None else MAX_VEHICLE_CLASSES
         if not 0 <= index < limit:
            self.err(f"vehicle: class {index} does not exist (0-{limit - 1})", s)
            index = 0
         e.op("Vehicle")
         e.u8(index)
         return True
      return None

   def f2_emit(self, e: Emitter, s: Node, ctx: dict[str, Any]) -> bool | None:
      """!Emits an F2 statement (darkness, flight, escape, field_action).
      @param e    emitter
      @param s    the statement
      @param ctx  compile context ("waits" is set by the statements that wait)
      @return True when emitted, None for another statement
      """
      k = s["k"]
      if k == "darkness":
         level = self.value(s["level"])
         # C8: the world's darkest level (its darkness_levels), else the engine's most
         darkest = (self.world_field or {}).get("darkness_levels", MAX_DARKNESS)
         if level is not None and not 0 <= level <= darkest:
            self.err(f"darkness: level {level} (0 lit to {darkest}, the world's darkest)", s)
         e.op("Darkness")
         self.emit_operand(e, s["level"])
         return True
      if k == "flight":
         points = [fp["name"] for fp in (self.world_field or {}).get("flight_points", [])]
         index = 0
         if s["point"] is not None:
            if s["point"] not in points:
               self.err(f"flight {s['kind']}: {s['point']!r} is not a flight point of the manifest's world "
                        f"({', '.join(points) or 'none: the world needs flight_points'})", s)
            else:
               index = points.index(s["point"])
         elif not points:
            self.err("flight menu: the manifest's world has no flight_points", s)
         e.op("Flight")
         e.u8(FLIGHT_KINDS.index(s["kind"]))
         if s["kind"] == "menu":
            e.u16(self.var_id(s["result"], s))
            ctx["waits"] = True
         else:
            e.u8(index)
            if s["kind"] == "visit":
               e.u8(TRANSITION_DEFAULT if s["transition"] == "default" else TRANSITIONS.index(s["transition"]))
               ctx["waits"] = True
         return True
      if k == "escape":
         if not (self.world_field or {}).get("escape"):
            self.err("escape: no map of the manifest's world has escape_point (nothing records an escape point)",
                     s)
         e.op("Escape")
         e.u8(TRANSITION_DEFAULT if s["transition"] == "default" else TRANSITIONS.index(s["transition"]))
         e.u16(self.var_id(s["result"], s) if s["result"] is not None else NO_VAR)
         ctx["waits"] = True
         return True
      if k == "field_action":
         names = [a["name"] for a in self.field_actions]
         index = 0
         if s["name"] not in names:
            self.err(f"field_action: {s['name']!r} is not a field action of the battle manifest's structure "
                     f"({', '.join(names) or 'none: the manifest needs its battle'})", s)
         else:
            index = names.index(s["name"])
         e.op("FieldAction")
         e.u8(FIELD_ACTION_KINDS.index(s["kind"]))
         e.u8(index)
         e.u16(self.var_id(s["result"], s))
         return True
      if k == "nursery":
         # B2: the battle manifest's nursery (capsule/script/nursery_port.h); the nickname prompt anywhere
         if s["kind"] not in NURSERY_ANYWHERE and not self.eggs["nursery"]:
            self.err(f"nursery {s['kind']}: the battle manifest's structure has no nursery", s)
         e.op("Nursery")
         e.u8(NURSERY_KINDS.index(s["kind"]))
         if s["arg"] is not None:
            self.emit_operand(e, s["arg"])
         if s["kind"] == "withdraw":
            e.u16(self.var_id(s["money"], s) if s["money"] is not None else NO_VAR)
         e.u16(self.var_id(s["result"], s))
         if s["kind"] in NURSERY_WAITING:
            ctx["waits"] = True
         return True
      if k == "combine":
         # B1: a combination method of the battle manifest's structure (capsule/script/combine_port.h)
         methods = [m["name"] for m in self.combine_methods]
         index = 0
         if s["name"] not in methods:
            self.err(f"combine: {s['name']!r} is not a combination method of the battle manifest's structure "
                     f"({', '.join(methods) or 'none: the manifest needs its battle with combine_methods'})", s)
         else:
            index = methods.index(s["name"])
            m = self.combine_methods[index]
            if s["kind"] != "open" and not m["min"] <= len(s["members"]) <= m["max"]:
               self.err(f"combine {s['kind']} {s['name']}: {len(s['members'])} members (the method takes "
                        f"{m['min']} to {m['max']})", s)
         if len(s["members"]) > MAX_COMBINE_MEMBERS:
            self.err(f"combine: at most {MAX_COMBINE_MEMBERS} members", s)
         e.op("Combine")
         e.u8(COMBINE_KINDS.index(s["kind"]))
         e.u8(index)
         if s["kind"] != "open":
            e.u8(len(s["members"]))
            for mem in s["members"]:
               self.emit_operand(e, mem)
         e.u16(self.var_id(s["result"], s))
         if s["kind"] in COMBINE_WAITING:
            ctx["waits"] = True
         return True
      return None

   def route_step_id(self, name: str) -> int:
      """!A game route step's ID for the route compiler (I4: `step NAME` declarations, in order).
      @param name  the step's name
      @return its ID
      @throws rc.RouteError when it is not declared
      """
      if name not in self.route_steps:
         raise rc.RouteError(f"unknown game route step {name!r} (declare it with 'step {name};')")
      return self.route_steps.index(name)

   def i4_emit(self, e: Emitter, s: Node, ctx: dict[str, Any]) -> bool | None:
      """!Emits an I4 statement (D-29: lock / release, wait_msg, remove, visible, set_template, door, weather, fade,
      set_metatile, respawn, set_warp, sound, wait_sound, mart).
      @param e    emitter
      @param s    the statement
      @param ctx  compile context ("waits" is set by the statements that wait)
      @return True when emitted, None for another statement
      """
      k = s["k"]
      if k in ("lock", "release"):
         e.op("Lock" if k == "lock" else "Release")
         e.u8(LOCK_ALL if s["all"] else LOCK_SELF)
         return True
      if k in ("wait_msg", "wait_state"):
         e.op("WaitMessage" if k == "wait_msg" else "WaitState")
         ctx["waits"] = True
         return True
      if k in ("remove", "visible", "set_template"):
         v = self.value(s["obj"])
         if v is not None:
            self.check_object(v, s["obj"], s)
         word = s["obj"].get("obj")
         if word == "camera" or (word == "player" and k != "visible"):
            self.err(f"{k} takes a map object" + (" or the player" if k == "visible" else ""), s)
         e.op({"remove": "ObjectRemove", "visible": "ObjectVisible", "set_template": "ObjectTemplate"}[k])
         self.emit_operand(e, s["obj"])
         if k == "visible":
            e.u8(1 if s["on"] else 0)
         elif k == "set_template":
            e.u8(TEMPLATE_KINDS.index(s["kind"]))
            for side in (s["a"], s["b"]):
               if side is not None:
                  self.emit_operand(e, side)
         return True
      if k == "door":
         e.op("Door")
         e.u8(DOOR_KINDS.index(s["kind"]))
         if s["kind"] == "wait":
            ctx["waits"] = True
         else:
            self.emit_operand(e, s["x"])
            self.emit_operand(e, s["y"])
         return True
      if k == "weather":
         e.op("Weather")
         e.u8(WEATHER_KINDS.index(s["kind"]))
         if s["value"] is not None:
            self.emit_operand(e, s["value"])
         return True
      if k == "fade":
         if not 0 <= s["speed"] <= U8_MAX:
            self.err("fade speed is 0-255 (frames per step; 0: the default)", s)
         e.op("Fade")
         e.u8(FADE_KINDS.index(s["kind"]))
         e.u8(s["speed"] & U8_MASK)
         e.u8(FADE_SWAP if s["swap"] else 0)
         ctx["waits"] = True
         return True
      if k == "set_metatile":
         e.op("SetMetatile")
         for part in ("x", "y", "id", "solid"):
            self.emit_operand(e, s[part])
         return True
      if k == "respawn":
         e.op("Respawn")
         self.emit_operand(e, s["value"])
         return True
      if k == "override":
         sets = (self.world_field or {}).get("override_sets", [])
         if s["set"] not in sets:
            self.err(f"override: {s['set']!r} is not an override set of the manifest's world "
                     f"({', '.join(sets) or 'none'})", s)
            return True
         e.op("Override")
         e.u8(OVERRIDE_KINDS.index(s["kind"]))
         e.u16(sets.index(s["set"]))
         return True
      if k == "set_warp":
         known = self.map_names
         if known is not None and s["map"] not in known:
            self.err(f"set_warp: {s['map']!r} is not a map of the world", s)
         e.op("SetWarp")
         e.u8(SET_WARP_KINDS.index(s["kind"]))
         e.u32(asset_key(f"map:{s['map']}", "map"))
         self.emit_operand(e, s["x"])
         self.emit_operand(e, s["y"])
         return True
      if k == "sound":
         args = list(s["args"])
         if len(args) > SOUND_KINDS[s["kind"]]:
            self.err(f"sound {s['kind']} takes {SOUND_KINDS[s['kind']]} operand(s)", s)
         zero = Node("num", Token("number", "0", s["line"], s["col"]), value=0)
         args += [zero] * (SOUND_KINDS[s["kind"]] - len(args))
         e.op("Sound")
         e.u8(list(SOUND_KINDS).index(s["kind"]))
         e.u8(len(args))
         for a in args:
            self.emit_operand(e, a)
         return True
      if k == "wait_sound":
         e.op("WaitSound")
         e.u8(SOUND_WAITS.index(s["kind"]))
         ctx["waits"] = True
         return True
      if k == "mart_open":
         mart = self.marts.get(s["name"])
         if mart is None:
            self.err(f"unknown mart {s['name']!r}", s)
            items: list[int] = []
         else:
            items = []
            for it in mart["items"]:
               v = self.value(it)
               if v is None or not 0 <= v <= U16_MASK:
                  self.err("a mart's items are constants 0-65535", it)
                  v = 0
               items.append(v)
         if not 0 <= s["kind"] <= U8_MAX:
            self.err("a mart kind is 0-255", s)
         e.op("Mart")
         e.u8(s["kind"] & U8_MASK)
         e.u16(len(items))
         for v in items:
            e.u16(v)
         ctx["waits"] = True
         return True
      return None

   def emit_item(self, e: Emitter, item: Node, s: Node) -> None:
      """!Emits an item operand: an item name of the battle manifest (its id as a constant) or an operand.
      @param e     emitter
      @param item  the item node (item_name or an operand)
      @param s     the statement (errors)
      @return None
      """
      if item["k"] != "item_name":
         self.emit_operand(e, item)
         return
      names = self.inventory_names[0] if self.inventory_names is not None else None
      if names is None:
         self.err(f"{s['kind']}: item names need the manifest's battle (or give an id)", s)
         iid = 0
      elif item["name"] not in names:
         self.err(f"{s['kind']}: {item['name']!r} is not an item of the battle manifest", s)
         iid = 0
      else:
         iid = names.index(item["name"]) + 1   # item ids count from 1 (0: no item)
      self.emit_operand(e, Node("num", Token("number", str(iid), s["line"], s["col"]), value=iid))

   def role_ref(self, s: Node) -> tuple[int, tuple[int, int] | None]:
      """!The role a role statement names: a role name, a constant, or a role ID.
      @param s  the role_get / role_set node
      @return (role ID, (width, count) or None when only the ID is known)
      """
      roles = self.field.roles
      name = s["role_name"]
      if name is not None and name in roles:
         rid, width, count = roles[name]
         return rid, (width, count)
      if name is not None and name not in self.consts:
         self.err(f"{s['k']}: {name!r} is not a record role ({', '.join(roles)})", s)
         return 0, None
      rid = self.consts[name]["value"] if name is not None else s["role"]
      by_id = {r: (width, count) for r, width, count in roles.values()}
      if rid in by_id:
         return rid, by_id[rid]
      if self.record_layout is None and CUSTOM_ROLE_FIRST <= rid < CUSTOM_ROLE_FIRST + MAX_CUSTOM_ROLES:
         return rid, None   # a custom role of a layout the compiler does not know (no "battle")
      self.err(f"{s['k']}: role {rid} is not a record role of the layout", s)
      return 0, None

   # Whole program.

   def compile_script(self, name: str, body: Iterable[Node], node: Node) -> tuple[bytes, set[str], bool, bool]:
      """!Compiles one script.
      @param name  script name
      @param body  statement nodes
      @param node  declaration node
      @return (bytes, calls, waits_directly, uses_self_switches); I4: the scripts it jumps to are in self.jumps
      """
      e = Emitter()
      ctx = {"calls": set(), "waits": False, "loose": set()}
      self.uses_self = False
      jumps = set()
      if self.statements(e, body, ctx):
         e.op("End")   # falling off the end ends the script
      # Patch each branch target with its label's offset.
      for pos, label, n in e.fixups:
         if label not in e.labels and pos in e.goto_sites and label in self.scripts:
            # I4: a goto to another script: a Jump (its frame takes the other script; nothing returns here)
            e.code[pos - 1] = OP["Jump"]
            struct.pack_into("<H", e.code, pos, self.script_ids[label])
            self.used.add(("script", label))
            jumps.add(label)
            continue
         if label not in e.labels:
            self.err(f"unknown label {label!r}", n or node)
            continue
         struct.pack_into("<H", e.code, pos, e.labels[label])
      if len(e.code) > MAX_SCRIPT_BYTES:
         self.err(f"script {name} is {len(e.code)} bytes; the limit is 65,535", node)
      self.jumps[name] = jumps | ctx["loose"]
      return bytes(e.code), ctx["calls"], ctx["waits"], self.uses_self

   def call_graph_checks(self, calls: Mapping[str, Iterable[str]],
                         jumps: Mapping[str, Iterable[str]] | None = None) -> dict[str, int]:
      """!Recursion and depth checks over the call graph (B-1). I4: a jump (goto naming a script) takes the frame of
      the script it leaves, so it adds no depth, and a loop of jumps alone is no recursion; a cycle with a call in it
      is. Iterative (Tarjan's strongly connected components), so long chains of jumps need no deep recursion.
      @param calls  {script: set of scripts it calls}
      @param jumps  {script: set of scripts it jumps to}, or None
      @return {script: deepest chain length from it}
      """
      jumps = jumps or {}
      order = list(self.order["script"])
      edges = {n: sorted(set(calls.get(n, ())) | set(jumps.get(n, ()))) for n in order}
      index: dict[str, int] = {}
      low: dict[str, int] = {}
      on_stack: set[str] = set()
      stack: list[str] = []
      comps: list[list[str]] = []   # in reverse topological order (each after the components it reaches)
      counter = 0
      for root in order:
         if root in index:
            continue
         work = [(root, 0)]
         while work:
            v, i = work.pop()
            if i == 0:
               index[v] = low[v] = counter
               counter += 1
               stack.append(v)
               on_stack.add(v)
            succ = edges.get(v, [])
            if i < len(succ):
               work.append((v, i + 1))
               w = succ[i]
               if w not in edges:
                  continue
               if w not in index:
                  work.append((w, 0))
               elif w in on_stack:
                  low[v] = min(low[v], index[w])
               continue
            if low[v] == index[v]:
               comp = []
               while True:
                  w = stack.pop()
                  on_stack.discard(w)
                  comp.append(w)
                  if w == v:
                     break
               comps.append(comp)
            if work:
               parent = work[-1][0]
               low[parent] = min(low[parent], low[v])
      depth: dict[str, int] = {}
      for comp in comps:
         members = set(comp)
         cyclic = len(comp) > 1 or comp[0] in set(edges.get(comp[0], ()))
         if cyclic:
            for n in sorted(members):
               inner = [t for t in sorted(calls.get(n, ())) if t in members]
               if inner:
                  # the cycle: the call, then the way back to n inside the component (breadth first)
                  back = {inner[0]: None}
                  queue = [inner[0]]
                  while queue and n not in back:
                     cur = queue.pop(0)
                     for t in edges.get(cur, ()):
                        if t in members and t not in back:
                           back[t] = cur
                           queue.append(t)
                  path = [n]
                  cur = back.get(n)
                  while cur is not None:
                     path.insert(1, cur)
                     cur = back.get(cur)
                  self.err("recursion: " + " -> ".join(path + [n]) +
                           " (scripts may not call themselves, directly or indirectly; a goto to a script adds no "
                           "depth)", self.scripts[n])
                  break
         d = 1
         for n in comp:
            for t in calls.get(n, ()):
               if t not in members:
                  d = max(d, 1 + depth.get(t, 0))
            for t in jumps.get(n, ()):
               if t not in members:
                  d = max(d, depth.get(t, 0))
         for n in comp:
            depth[n] = d
      for n in order:
         if depth.get(n, 0) > self.stack_depth:
            self.err(f"script {n} calls {depth[n]} deep; the VM stack holds {self.stack_depth} "
                     f"(stack_depth)", self.scripts[n])
      return depth

   def run(self) -> Compiler:
      """!Compiles everything.
      @return self (check errors() before using the result)
      """
      try:
         paths = self.read_manifest()
      except ScriptError as e:
         self.err(e.message)
         return self
      files = self.parse_all(paths)
      # F1: the timed field effects' implicit variables and expiry scripts (declared like the sources' own)
      if self.field_effects:
         implicit = Parser(field_effect_source(self.field_effects), self.field_effect_file).file_items()
         stamp(implicit, self.field_effect_file)
         files.append((self.field_effect_file, implicit))
         for e in self.field_effects:
            self.used.add(("var", field_var_name(e["name"])))
      # F2: the field actions' member variable and wrappers, the flight points' flags, the escape point's variables;
      # C8: the darkness variable (a dark map, or a source using `darkness`)
      dark = bool((self.world_field or {}).get("dark")) or any(has_statement(items, DARKNESS_WORD)
                                                              for _, items in files)
      if self.field_actions or (self.world_field or {}).get("flight_points") or (self.world_field or {}).get("escape") \
            or dark:
         from battlec.field import ANSWER_VAR, MEMBER_VAR
         implicit = Parser(field_action_source(self.field_actions, self.world_field, dark),
                           self.field_action_file).file_items()
         stamp(implicit, self.field_action_file)
         files.append((self.field_action_file, implicit))
         self.used |= {("var", MEMBER_VAR), ("var", ANSWER_VAR), ("var", DARKNESS_VAR)}
         self.used |= {("var", n) for n in ESCAPE_VARS}
         self.used |= {("flag", FLIGHT_FLAG_PREFIX + fp["name"])
                       for fp in (self.world_field or {}).get("flight_points", [])}
         from battlec.field import game_refs
         self.used |= set(game_refs(self.field_actions))   # the requirements read them
      # F3: the step effects' counters (and their scripts' member / outcome variables)
      if self.step_effects:
         from battlec.field import MEMBER_STEP_VAR, OUTCOME_STEP_VAR, step_var_name
         implicit = Parser(step_effect_source(self.step_effects), self.step_effect_file).file_items()
         stamp(implicit, self.step_effect_file)
         files.append((self.step_effect_file, implicit))
         self.used |= {("var", step_var_name(e["name"])) for e in self.step_effects}
         self.used |= {("var", MEMBER_STEP_VAR), ("var", OUTCOME_STEP_VAR)}
      # B2: the eggs' member variable (the hatch script reads it)
      if self.eggs["eggs"]:
         from battlec.nursery import EGG_MEMBER_VAR
         egg_file = f"{self.step_effect_file.split(':')[0]}: eggs"
         implicit = Parser(egg_source(), egg_file).file_items()
         stamp(implicit, egg_file)
         files.append((egg_file, implicit))
         self.used.add(("var", EGG_MEMBER_VAR))
      # DS1: the flags and variables the world's override sets persist through (the field reads them)
      self.used |= {(kind, name) for kind, name in (self.world_field or {}).get("override_persist", [])}
      for _, items in files:
         self.declare(items)
      # IDs: flags bank by bank, the rest in declaration order.
      ## Flag name -> ID (bank by bank).
      self.flag_ids = {}
      flag_first = []
      for life in LIFETIMES:
         flag_first.append(len(self.flag_ids))
         for n in self.order["flag"]:
            if self.flags[n]["lifetime"] == life:
               self.flag_ids[n] = len(self.flag_ids)
      flag_first.append(len(self.flag_ids))
      if len(self.flag_ids) > DEP_VAR or len(self.vars) > DEP_VAR:
         self.err(f"too many flags or variables ({DEP_VAR:,} of each at most)")
      ## Variable name -> ID.
      self.var_ids = {n: i for i, n in enumerate(self.order["var"])}
      ## Native name -> ID.
      self.native_ids = {n: i for i, n in enumerate(self.order["native"])}
      ## Script name -> ID.
      self.script_ids = {n: i for i, n in enumerate(self.order["script"])}
      ## Inline text codes, each ending with 0xFF.
      self.text_pool = []
      ## Encoded inline text -> offset in text_pool.
      self.text_offsets = {}
      self.resolve_placeholders()
      codes, calls, waits, selfs = {}, {}, {}, {}
      for n in self.order["script"]:
         c, cl, w, sw = self.compile_script(n, self.scripts[n]["body"], self.scripts[n])
         codes[n], calls[n], waits[n], selfs[n] = c, cl, w, sw
      self.call_graph_checks(calls, self.jumps)
      # Waiting is inherited from callees (I4: and from the scripts a script jumps to).
      changed = True
      while changed:
         changed = False
         for n in self.order["script"]:
            reached = set(calls[n]) | self.jumps.get(n, set())
            if not waits[n] and any(waits.get(t) for t in reached):
               waits[n] = True
               changed = True
            if not selfs[n] and any(selfs.get(t) for t in reached):
               selfs[n] = True
               changed = True
      # Map triggers; conditions become extra scripts.
      triggers, deps, maps = [], [], []
      extra = []
      for mname in self.order["map"]:
         ms = self.maps[mname]
         first = len(triggers)
         on_frame = 0
         warp_into = 0
         for t in ms["entries"]:
            target = t["script"]
            if self.scripts.get(target) is None:
               self.err(f"unknown script {target!r}", t)
               continue
            self.used.add(("script", target))
            cond_id = NO_CONDITION
            dep_first = len(deps)
            dep_count = 0
            if t["kind"] in IMMEDIATE_KINDS and waits[target]:
               self.err(f"{t['kind']} script {target} waits (messages, menus, delays or waiting "
                        f"natives); it must run to its end at once", t)
            if t["kind"] in CONDITION_KINDS:
               on_frame += 1 if t["kind"] == "on_frame" else 0
               e = Emitter()
               false = e.new_label()
               self.jump(e, t["cond"], false, False)
               e.op("TriggerTrue")
               e.place(false)
               e.op("End")
               # The condition script's labels are all its own, so every fixup resolves.
               for pos, label, _ in e.fixups:
                  struct.pack_into("<H", e.code, pos, e.labels[label])
               warp_into += 0 if t["kind"] == "on_frame" else 1
               cond_name = f"{mname}.on_frame.{on_frame}" if t["kind"] == "on_frame" else \
                  f"{mname}.on_warp_into.{warp_into}"
               cond_id = len(self.order["script"]) + len(extra)
               extra.append((cond_name, bytes(e.code)))
               found = set()
               self.deps_of(t["cond"], found)
               dynamic = sorted(n for k, n in found if k == "dynamic")
               if dynamic and t["kind"] == "on_frame":
                  self.err(f"on_frame conditions are re-checked only when flags and variables change, so "
                           f"they cannot read {', '.join(dynamic)}; use a parallel object instead", t)
               for kind, name in sorted(found):
                  if kind == "flag" and name in self.flag_ids:
                     deps.append(self.flag_ids[name])
                  elif kind == "var" and name in self.var_ids:
                     deps.append(DEP_VAR | self.var_ids[name])
               dep_count = len(deps) - dep_first
               if dep_count > MAX_TRIGGER_DEPS:
                  self.err(f"a trigger condition reads at most {MAX_TRIGGER_DEPS} flags and variables", t)
               if dep_count == 0 and t["kind"] == "on_frame":
                  self.warn("this condition reads no flag or variable: it is decided once, on entry", t)
            triggers.append((self.script_ids[target], cond_id, dep_first, dep_count,
                             TRIGGER_KINDS.index(t["kind"])))
         if on_frame > MAX_TRIGGERS:
            self.err(f"map {mname} has {on_frame} on_frame triggers; at most {MAX_TRIGGERS}", ms)
         if len(triggers) - first > MAX_MAP_ENTRIES:
            self.err(f"map {mname} has {len(triggers) - first} entries; at most 255", ms)
         maps.append((asset_key(f"map:{mname}", "map"), first, len(triggers) - first, mname))
      if len(triggers) > MAX_BANK_ENTRIES or len(deps) > MAX_BANK_ENTRIES:
         self.err("too many map triggers or trigger dependencies in the bank (65,535 at most)")
      maps.sort(key=lambda m: m[0])
      # Unused declarations (I4: unless the manifest turns the warnings off).
      for kind, table in (("flag", self.flags), ("var", self.vars)) if self.warn_unused else ():
         used = self.used
         world_uses = self.world_uses
         for n in self.order[kind]:
            if (kind, n) not in used and (kind, n) not in world_uses:
               self.warn(f"{kind} {n} is never used", table[n])
      # Storage layout: flags bit-packed per bank; variables by width, aligned.
      flag_bytes = [(flag_first[i + 1] - flag_first[i] + BITS_PER_BYTE - 1) // BITS_PER_BYTE
                    for i in range(len(LIFETIMES))]
      var_layout = {}
      var_bytes = [0] * len(LIFETIMES)
      for li, life in enumerate(LIFETIMES):
         members = [n for n in self.order["var"] if self.vars[n]["lifetime"] == life]
         members.sort(key=lambda n: -self.vars[n]["width"])
         off = 0
         for n in members:
            w = self.vars[n]["width"]
            off = (off + w - 1) // w * w   # round up to the width (natural alignment)
            var_layout[n] = off
            off += w
         var_bytes[li] = off
         if off > MAX_BANK_BYTES or flag_bytes[li] > MAX_BANK_BYTES:
            self.err(f"the {life} bank is larger than 64 KB")
      # Code blob with identical scripts stored once.
      blob = bytearray()
      offsets = {}
      infos = []
      names = list(self.order["script"]) + [n for n, _ in extra]
      all_code = [codes[n] for n in self.order["script"]] + [c for _, c in extra]
      all_waits = [waits[n] for n in self.order["script"]] + [False] * len(extra)
      for code, w in zip(all_code, all_waits):
         if code not in offsets:
            while len(blob) % CODE_ALIGNMENT:
               blob.append(0)
            offsets[code] = len(blob)
            blob += code
         infos.append((offsets[code], len(code), 1 if w else 0))
      if len(infos) > MAX_BANK_ENTRIES:
         self.err("too many scripts (65,535 at most)")
      self.result = {
         "code": bytes(blob), "scripts": infos, "names": names, "text": self.text_pool,
         "flag_first": flag_first, "flag_bytes": flag_bytes, "var_bytes": var_bytes,
         "vars": [(self.vars[n]["lo"], self.vars[n]["hi"], var_layout[n],
                   LIFETIMES.index(self.vars[n]["lifetime"]), self.vars[n]["width"]) for n in self.order["var"]],
         "maps": maps, "triggers": triggers, "deps": deps, "stack_depth": self.stack_depth,
         "flag_names": sorted(self.flag_ids, key=self.flag_ids.get), "var_names": list(self.order["var"]),
         "natives": list(self.order["native"]), "script_count": len(self.order["script"]),
         "waits": dict(waits), "uses_self": {n for n, v in selfs.items() if v},
         "placeholders": list(self.placeholder_table),
         "placeholder_names": list(self.field.bindings) if self.field is not None else [],
         "name_bytes": self.name_bytes,
         "field_effects": self.field_effect_records(),
         "field_actions": self.field_action_records(), "field_action_refs": self.field_action_ref_records(),
         "flight_points": self.flight_point_records(), "f2_vars": self.f2_var_ids(),
         "step_effects": self.step_effect_records(), "step_vars": self.step_var_ids(),
         "hatch": self.hatch_record(),
      }
      return self

   def field_effect_records(self) -> list[tuple[int, int, int, int, int, int]]:
      """!The bank's timed field effect records (F1; capsule::script::FieldEffectInfo): the label's offset in the
      text pool, the implicit variable, the expiry script (or NO_VAR), the unit, the stack rule, the flags.
      @return the records, in the structure's order
      """
      from battlec.field import end_script_name
      out = []
      for fx in self.field_effects:
         try:
            codes = encode_text(fx["label"], "label", self.codepage)
         except (AssetError, text_encoder.TextError) as ex:
            self.err(f"{self.field_effect_file}: {fx['name']}: {ex}")
            codes = [CODE_END]
         script = self.script_ids.get(end_script_name(fx["name"]), NO_VAR)
         out.append((self.pool_text(codes), self.var_ids.get(field_var_name(fx["name"]), 0), script,
                     fx["unit_id"], fx["stack_id"], fx["flags"]))
      return out

   def step_effect_records(self) -> list[tuple[int, int, int, int, int, list[int], str]]:
      """!The bank's step effect records (F3; capsule::script::StepEffectInfo): the label's offset in the text pool,
      the implicit variable, every, the script (or NO_SCRIPT_ID), the damage rule, the message ids.
      @return [(label, var, every, script, damage, messages, name)], in the structure's order
      """
      from battlec.field import step_var_name
      out = []
      effects = self.step_effects   # a local name (Doxygen 1.12 reads `in self.attr` as a member)
      for e in effects:
         where = f"{self.step_effect_file}: {e['name']}"
         try:
            codes = encode_text(e["label"], "label", self.codepage)
         except (AssetError, text_encoder.TextError) as ex:
            self.err(f"{where}: {ex}")
            codes = [CODE_END]
         script = NO_SCRIPT_ID
         if e["script"] is not None:
            if e["script"] not in self.script_ids:
               self.err(f"{where}: its script {e['script']!r} is not declared")
            script = self.script_ids.get(e["script"], NO_SCRIPT_ID)
         out.append((self.pool_text(codes), self.var_ids.get(step_var_name(e["name"]), 0), e["every"], script,
                     e["damage_id"], list(e["message_ids"]), e["name"]))
      return out

   def hatch_record(self) -> tuple[int, int] | None:
      """!The bank's egg members (B2): the hatch script and the egg_member variable.
      @return (ScriptId or NO_SCRIPT_ID, VarId), or None without eggs
      """
      from battlec.nursery import EGG_MEMBER_VAR
      if not self.eggs["eggs"]:
         return None
      script = NO_SCRIPT_ID
      name = self.eggs["hatch_script"]
      if name is not None:
         if name not in self.script_ids:
            self.err(f"eggs: the hatch script {name!r} is not declared")
         script = self.script_ids.get(name, NO_SCRIPT_ID)
      return script, self.var_ids.get(EGG_MEMBER_VAR, NO_VAR)

   def step_var_ids(self) -> dict[str, int]:
      """!The VarIds of F3's member and outcome variables (NO_VAR where absent).
      @return {"member", "outcome"}
      """
      from battlec.field import MEMBER_STEP_VAR, OUTCOME_STEP_VAR
      return {"member": self.var_ids.get(MEMBER_STEP_VAR, NO_VAR), "outcome": self.var_ids.get(OUTCOME_STEP_VAR,
                                                                                               NO_VAR)}

   def field_action_records(self) -> list[tuple[int, int, int, int, int, str]]:
      """!The bank's field action records (F2; capsule::script::FieldActionInfo): the label's offset in the text pool,
      the script it runs (an asking action: its wrapper), the target kind and index, the flags; with the target's
      resolution checked against the world.
      @return [(label, script, target kind, target index, flags, name)], in the structure's order
      """
      from battlec.field import TARGET_KINDS, ask_script_name
      out = []
      wf = self.world_field
      lists = {"behavior": "behaviors", "object": "object_kinds", "tag": "map_tags"}
      actions = self.field_actions   # a local name (Doxygen 1.12 reads `in self.attr` as a member)
      for a in actions:
         where = f"{self.field_action_file}: {a['name']}"
         try:
            codes = encode_text(a["label"], "label", self.codepage)
         except (AssetError, text_encoder.TextError) as ex:
            self.err(f"{where}: {ex}")
            codes = [CODE_END]
         value = 0
         if a["target"] != TARGET_KINDS[0]:
            names = (wf or {}).get(lists[a["target"]])
            if names is None:
               self.err(f"{where}: its target {a['target']} {a['target_name']!r} needs the manifest's world")
            elif a["target_name"] not in names:
               self.err(f"{where}: its target {a['target']} {a['target_name']!r} is not one of the world's "
                        f"{lists[a['target']]} ({', '.join(names) or 'none'})")
            else:
               # object kinds are index + 1 (map::ObjectTemplate::kind; 0 = no kind)
               value = names.index(a["target_name"]) + (1 if a["target"] == "object" else 0)
         if a["script"] not in self.script_ids:
            self.err(f"{where}: its script {a['script']!r} is not declared")
         script = self.script_ids.get(ask_script_name(a["name"]) if a["ask"] else a["script"], NO_SCRIPT_ID)
         out.append((self.pool_text(codes), script, TARGET_KINDS.index(a["target"]), value,
                     ACTION_ASKS if a["ask"] else 0, a["name"]))
      return out

   def field_action_ref_records(self) -> list[tuple[int, int, str]]:
      """!The flags and variables the field actions' requirements read (F2; capsule::script::FieldActionRef, in
      tools/battlec/field.py game_refs() order: the battle data's rules::field_state() indices).
      @return [(id, kind (0 flag, 1 variable), name)]
      """
      from battlec.field import GAME_REF_KINDS, game_refs
      out = []
      for kind, name in game_refs(self.field_actions):
         ids = self.flag_ids if kind == GAME_REF_KINDS[0] else self.var_ids
         if name not in ids:
            self.err(f"{self.field_action_file}: a requirement reads {kind}.{name}, which the scripts do not declare")
         out.append((ids.get(name, 0), GAME_REF_KINDS.index(kind), f"{kind}.{name}"))
      return out

   def flight_point_records(self) -> list[tuple[int, int, int, int, int, int, str]]:
      """!The world's flight points (F2; capsule::script::FlightPointInfo).
      @return [(label, map key, x, y, flag, facing, name)]
      """
      out = []
      for fp in (self.world_field or {}).get("flight_points", []):
         try:
            codes = encode_text(fp["label"], "label", self.codepage)
         except (AssetError, text_encoder.TextError) as ex:
            self.err(f"{self.field_action_file}: flight point {fp['name']}: {ex}")
            codes = [CODE_END]
         out.append((self.pool_text(codes), asset_key(f"map:{fp['map']}", "flight point"), fp["x"], fp["y"],
                     self.flag_ids.get(FLIGHT_FLAG_PREFIX + fp["name"], 0), fp["facing"], fp["name"]))
      return out

   def f2_var_ids(self) -> dict[str, int]:
      """!The VarIds of F2's implicit variables (NO_VAR where absent).
      @return {"member", "escape" [map_hi, map_lo, x, y, facing], "darkness" (C8)}
      """
      from battlec.field import MEMBER_VAR
      return {"member": self.var_ids.get(MEMBER_VAR, NO_VAR),
              "escape": [self.var_ids.get(n, NO_VAR) for n in ESCAPE_VARS],
              "darkness": self.var_ids.get(DARKNESS_VAR, NO_VAR)}

   # Output.

   def save_bytes(self) -> int:
      """!Bytes the flags and variables take in a save (C10: the session lifetime's are never saved).
      @return bytes
      """
      r = self.result
      saved = LIFETIMES.index(SESSION_LIFETIME)
      return sum(r["flag_bytes"][:saved]) + sum(r["var_bytes"][:saved]) + r["name_bytes"]

   def emit_cpp(self, source_name: str, debug_names: bool = False) -> str:
      """!The generated bank.
      @param source_name  manifest name for the header comment
      @param debug_names  include script, flag and variable names
      @return C++ source text
      """
      r = self.result
      L = [f"// Generated by tools/script_compiler.py from {source_name}. Do not edit.",
           '#include "capsule/script/data.h"', '#include "capsule/core/memory.h"', "",
           "namespace capsule::script {", "namespace {", ""]

      def arr(ctype: str, name: str, values: Sequence[Any], fmt: str = "{}", per_line: int = DEFAULT_PER_LINE,
              align: str = "") -> str:
         """!Emits an array, or nothing when empty.
         @param ctype     element type
         @param name      array name
         @param values    elements (already formatted when fmt is '{}')
         @param fmt       format per element
         @param per_line  elements per line
         @param align     alignment prefix
         @return the array name, or 'nullptr' when empty
         """
         if not values:
            return "nullptr"
         L.append(f"{align}const {ctype} {name}[] = {{")
         for i in range(0, len(values), per_line):
            L.append("    " + ", ".join(fmt.format(v) for v in values[i:i + per_line]) + ",")
         L.append("};")
         return name

      code = arr("uint8_t", "kCode", list(r["code"]), "0x{:02X}", BYTES_PER_LINE, "alignas(4) ")
      scripts = arr("ScriptInfo", "kScripts", [f"{{{o}u, {n}, {w}, 0}}" for o, n, w in r["scripts"]],
                    per_line=SCRIPTS_PER_LINE)
      text = arr("text::Code", "kText", r["text"], "0x{:02X}", BYTES_PER_LINE)
      vars_ = arr("VarInfo", "kVars", [f"{{{lo}, {hi}, {off}, Lifetime{{{li}}}, {w}}}"
                                      for lo, hi, off, li, w in r["vars"]], per_line=RECORDS_PER_LINE)
      maps = arr("MapScripts", "kMaps", [f"{{AssetKey{{0x{k:08X}u}}, {f}, {n}, 0}}" for k, f, n, _ in r["maps"]],
                 per_line=RECORDS_PER_LINE)
      trig = arr("MapTrigger", "kTriggers", [f"{{{s}, {c}, {df}, {dc}, TriggerKind{{{kd}}}}}"
                                             for s, c, df, dc, kd in r["triggers"]], per_line=RECORDS_PER_LINE)
      deps = arr("uint16_t", "kDeps", r["deps"], "0x{:04X}")
      # F1: FieldEffectInfo {label, var, expiry_script, unit, stack, flags, reserved}
      field_effects = arr("FieldEffectInfo", "kFieldEffects",
                          [f"{{{lb}u, {v}, 0x{sc:04X}, FieldEffectUnit{{{u}}}, FieldEffectStack{{{st}}}, {fl}, 0}}"
                           for lb, v, sc, u, st, fl in r["field_effects"]], per_line=RECORDS_PER_LINE)
      holders = arr("Placeholder", "kPlaceholders", [f"{{PlaceholderKind{{{k}}}, {pa}, {i}, {t}u}}"
                                                     for k, pa, i, t in r["placeholders"]],
                    per_line=RECORDS_PER_LINE)
      names_store = "nullptr"
      if r["name_bytes"]:
         L.append(f"CAPSULE_EWRAM_BSS alignas(4) uint8_t g_names[{r['name_bytes']}];")
         names_store = "g_names"
      flag_store, var_store = [], []
      for li, life in enumerate(LIFETIMES):
         for kind, sizes, store in (("flags", r["flag_bytes"], flag_store), ("vars", r["var_bytes"], var_store)):
            if sizes[li]:
               L.append(f"CAPSULE_EWRAM_BSS alignas(4) uint8_t g_{kind}_{life}[{sizes[li]}];")
               store.append(f"g_{kind}_{life}")
            else:
               store.append("nullptr")
      try:
         remaps, moves, remap_count = save_shape.emit(self, self.save_history, L)
      except save_shape.ShapeError as e:
         raise ValueError(str(e)) from e
      names = ("nullptr", "nullptr", "nullptr")
      if debug_names:
         def strs(name: str, values: Sequence[str]) -> str:
            """!Emits a name table.
            @param name    array name
            @param values  strings
            @return the array name, or nullptr when empty
            """
            if not values:
               return "nullptr"
            L.append(f"const char* const {name}[] = {{")
            for v in values:
               L.append(f'    "{v}",')
            L.append("};")
            return name
         names = (strs("kScriptNames", r["names"]), strs("kFlagNames", r["flag_names"]),
                  strs("kVarNames", r["var_names"]))
      c10 = [f"    {self.vm_flags},"] if self.vm_flags else []   # C10: Bank::vm_flags
      i4 = [f"    {self.object_base},"] if self.object_base or c10 else []   # I4: Bank::object_base
      b2 = self.b2_initializer(bool(i4))
      f3 = self.f3_initializer(L, bool(b2))
      f2 = self.f2_initializer(L, bool(f3))
      L += ["", "} // namespace", "", "const Bank kBank = {",
            f"    {code}, {scripts}, {text}, {vars_}, {maps}, {trig}, {deps},",
            f"    {{{', '.join(flag_store)}}},",
            f"    {{{', '.join(var_store)}}},",
            f"    {{{', '.join(map(str, r['flag_bytes']))}}},",
            f"    {{{', '.join(map(str, r['var_bytes']))}}},",
            f"    {{{', '.join(map(str, r['flag_first']))}}},",
            f"    {len(r['scripts'])}, {len(r['vars'])}, {len(r['maps'])}, {r['stack_depth']}, "
            f"{len(r['placeholders'])},",
            f"    {', '.join(names)},",
            f"    {holders}, {names_store}, {r['name_bytes']},",
            f"    {remap_count},",
            f"    {save_shape.hashes_initializer(self)},",
            f"    {remaps}, {moves},",
            f"    {field_effects}, {len(r['field_effects'])},",
            *f2,
            *f3,
            *b2,
            *i4,
            *c10,
            "};", "", "} // namespace capsule::script", ""]
      return "\n".join(L)

   def b2_initializer(self, forced: bool = False) -> list[str]:
      """!The Bank's B2 members (the hatch script, the egg_member variable); nothing for a bank without eggs.
      @param forced  emit them even without eggs (I4 members follow them in the initializer)
      @return the initializer lines
      """
      h = self.result["hatch"]
      if h is None:
         return [f"    0x{NO_SCRIPT_ID:04X}, 0x{NO_VAR:04X},"] if forced else []
      return [f"    0x{h[0]:04X}, 0x{h[1]:04X},"]

   def f3_initializer(self, out: list[str], forced: bool = False) -> list[str]:
      """!The Bank's F3 members (the step effects, the member and outcome variables), with their array appended to
      the anonymous namespace part; nothing for a bank without step effects (its initializer stays as before F3).
      @param out     the lines so far, inside the anonymous namespace (the array is appended)
      @param forced  emit them even without step effects (B2 members follow them in the initializer)
      @return the initializer lines
      """
      r = self.result
      if not r["step_effects"]:
         return ["    nullptr, 0, 0xFFFF, 0xFFFF,"] if forced else []
      out.append("const StepEffectInfo kStepEffects[] = {")
      for lb, var, every, script, damage, msgs, name in r["step_effects"]:
         ids = ", ".join(f"0x{m:02X}" for m in msgs)
         out.append(f"    {{{lb}u, {var}, {every}, 0x{script:04X}, StepDamage{{{damage}}}, {{{ids}}}, {{0, 0}}}},   "
                    f"// {name}")
      out.append("};")
      v = r["step_vars"]
      return [f"    kStepEffects, {len(r['step_effects'])}, 0x{v['member']:04X}, 0x{v['outcome']:04X},"]

   def f2_initializer(self, out: list[str], forced: bool = False) -> list[str]:
      """!The Bank's F2 members (F2: field actions, their references, flight points, the member and escape
      variables), with their arrays appended to the anonymous namespace part; nothing for a bank without them (its
      initializer stays as before F2).
      @param out     the lines so far, inside the anonymous namespace (the arrays are appended)
      @param forced  emit them even without F2 data (F3 members follow them in the initializer)
      @return the initializer lines
      """
      r = self.result
      v = r["f2_vars"]
      if not forced and not (r["field_actions"] or r["flight_points"] or v["member"] != NO_VAR or
                             any(x != NO_VAR for x in v["escape"]) or v["darkness"] != NO_VAR):
         return []
      arrays: list[str] = []
      syms = []
      for ctype, name, rows in (
            ("FieldActionInfo", "kFieldActions",
             [f"{{{lb}u, 0x{sc:04X}, ActionTarget{{{tk}}}, {tv}, {fl}, {{0, 0, 0}}}},   // {n}"
              for lb, sc, tk, tv, fl, n in r["field_actions"]]),
            ("FieldActionRef", "kFieldActionRefs",
             [f"{{{i}, {kd}, 0}},   // {n}" for i, kd, n in r["field_action_refs"]]),
            ("FlightPointInfo", "kFlightPoints",
             [f"{{{lb}u, AssetKey{{0x{key:08X}u}}, {x}, {y}, {fg}, {fc}, 0}},   // {n}"
              for lb, key, x, y, fg, fc, n in r["flight_points"]])):
         if rows:
            arrays.append(f"const {ctype} {name}[] = {{")
            arrays.extend(f"    {row}" for row in rows)
            arrays.append("};")
            syms.append(name)
         else:
            syms.append("nullptr")
      out.extend(arrays)
      esc = ", ".join(f"0x{x:04X}" for x in [*v["escape"], NO_SCRIPT_ID])   # EscapeVars' reserved: kNoScriptId
      actions, refs, points = syms
      return [f"    {actions}, {refs}, {points},",
              f"    {len(r['field_actions'])}, {len(r['field_action_refs'])}, {len(r['flight_points'])}, "
              f"0x{v['member']:04X}, {{{esc}}}, 0x{v['darkness']:04X},"]

   def emit_ids(self, source_name: str) -> str:
      """!The generated ID header.
      @param source_name  manifest name for the header comment
      @return C++ header text
      """
      r = self.result
      L = [f"// Generated by tools/script_compiler.py from {source_name}. Do not edit.", "#pragma once", "",
           '#include "capsule/script/data.h"', "", "#include <cstdint>", "",
           "/// IDs of the build's scripts, flags, variables and natives (capsule::script::kBank).",
           "namespace script_ids {", ""]
      for i, n in enumerate(r["names"][:r["script_count"]]):
         L.append(f"inline constexpr capsule::script::ScriptId SCRIPT_{n.upper()}{{{i}}};")
      L.append("")
      for n in r["flag_names"]:
         L.append(f"inline constexpr capsule::script::FlagId FLAG_{n.upper()}{{{self.flag_ids[n]}}};")
      L.append("")
      for i, n in enumerate(r["var_names"]):
         L.append(f"inline constexpr capsule::script::VarId VAR_{n.upper()}{{{i}}};")
      L.append("")
      for i, n in enumerate(r["natives"]):
         L.append(f"inline constexpr uint16_t NATIVE_{n.upper()} = {i};")
      if r["placeholder_names"]:
         L.append("")
         for i, n in enumerate(r["placeholder_names"]):
            ident = n.upper().replace(":", "_")   # I4: a name's ':' part (str:1) as an identifier
            L.append(f"inline constexpr uint8_t PLACEHOLDER_{ident} = {i};   ///< Field placeholder {{{n}}}.")
      if self.record_layout is not None:
         L.append("")
         for n, (rid, _w, _c) in self.field.roles.items():
            L.append(f"inline constexpr uint8_t ROLE_{n.upper()} = 0x{rid:02X};   ///< Record role {n} (role_get).")
      if self.route_steps:   # I4: the game route steps (the step IDs field::game::route_step() receives)
         L.append("")
         for i, n in enumerate(self.route_steps):
            L.append(f"inline constexpr uint8_t ROUTE_STEP_{n.upper()} = {i};")
      if r["flight_points"]:   # F2: the flight points by index (the flight menu's order)
         L.append("")
         for i, fp in enumerate(r["flight_points"]):
            L.append(f"inline constexpr uint8_t FLIGHT_POINT_{fp[-1].upper()} = {i};")
      what = "Flags + variables + name slots." if r["name_bytes"] else "Flags + variables."
      L += ["", f"inline constexpr uint32_t kScriptSaveBytes = {self.save_bytes()};   ///< {what}",
            "", "} // namespace script_ids", ""]
      return "\n".join(L)


def stamp(items: Any, file: str) -> None:
   """!Writes the source file into every node (for diagnostics).
   @param items  nodes (recursively)
   @param file   source file
   @return None
   """
   if isinstance(items, list):
      for x in items:
         stamp(x, file)
   elif isinstance(items, dict):
      items["file"] = file
      for v in items.values():
         if isinstance(v, (list, dict)):
            stamp(v, file)


def load_asset_keys(path: str | PathLike[str], deps: list[str] | None = None) -> dict[int, Any]:
   """!The asset keys and kinds of a capsule.assets/1 manifest (for text:key checks; I4: with its includes and the
   texts of its text tables).
   @param path  manifest path
   @param deps  collects the files read, or None
   @return {key: kind}
   """
   import asset_compiler
   path = Path(path)
   m = json.loads(path.read_text(encoding="utf-8"))
   read: list[Path] = []
   try:
      merged, table_texts = asset_compiler.expand_manifest(m, path.parent, read)
   except asset_compiler.ManifestError as e:
      raise ValueError(str(e)) from None
   if deps is not None:
      deps += [str(p) for p in read]
   out = {}
   for a in merged.get("assets", []):
      if isinstance(a, dict) and isinstance(a.get("key"), str):
         try:
            out[asset_key(a["key"], "asset")] = a.get("kind")
         except KeyError_:
            pass
   for t in table_texts:
      try:
         out[asset_key(t["key"], "asset")] = "text"
      except KeyError_:
         pass
   return out


def load_map_names(path: str | PathLike[str]) -> set[str]:
   """!The map names of a world manifest (for map_scripts checks).
   @param path  world manifest path
   @return set of names
   """
   m = json.loads(Path(path).read_text(encoding="utf-8"))
   maps = m.get("maps", {})
   return set(maps) if isinstance(maps, dict) else set()


def load_vehicle_names(path: str | PathLike[str]) -> list[str]:
   """!The vehicle class names of a world manifest, in class order (A2; the
   map compiler checks them).
   @param path  world manifest path
   @return names (default: just "foot")
   """
   w = json.loads(Path(path).read_text(encoding="utf-8"))
   raw = w.get("vehicle_classes", ["foot"]) if isinstance(w, dict) else []
   raw = raw if isinstance(raw, list) else []
   return [str(c.get("name")) if isinstance(c, dict) else str(c) for c in raw]


def load_world_uses(path: str | PathLike[str]) -> tuple[set[tuple[str, str]], list[str]]:
   """!Flags and variables the world's maps use in events and objects (B-3, B-21).
   Unreadable map manifests are skipped (the map compiler reports them).
   @param path  world manifest path
   @return (set of ("flag" | "var", name), list of map manifest paths read)
   """
   path = Path(path)
   w = read_json_cached(path)
   uses, read = set(), []
   maps = w.get("maps", {})
   for rel in (maps.values() if isinstance(maps, dict) else []):
      p = path.parent / str(rel)
      try:
         m = read_json_cached(p)
      except (OSError, ValueError):
         continue
      read.append(str(p))
      if not isinstance(m, dict):
         continue
      ev = m.get("events") if isinstance(m.get("events"), dict) else {}
      items = [e for k in ("triggers", "signs") for e in (ev.get(k) or []) if isinstance(e, dict)]
      for o in (m.get("objects") or []):
         if not isinstance(o, dict):
            continue
         items.append(o)
         if isinstance(o.get("spotter"), dict) and isinstance(o["spotter"].get("until"), str):
            uses.add(("flag", o["spotter"]["until"]))
         for pg in (o.get("pages") or []):
            if isinstance(pg, dict) and isinstance(pg.get("when"), dict):
               items.append(pg["when"])
         route = o.get("route")
         steps = route.get("steps") if isinstance(route, dict) else None
         for st in (steps if isinstance(steps, list) else []):
            words = st.split() if isinstance(st, str) else []
            if len(words) == ROUTE_FLAG_WORDS and words[0] in ("set", "clear"):
               uses.add(("flag", words[1]))
      for e in items:
         if isinstance(e.get("flag"), str):
            uses.add(("flag", e["flag"]))
         if isinstance(e.get("var"), str):
            uses.add(("var", e["var"]))
   return uses, read


def compile_file(path: str | PathLike[str], sources: Mapping[str, str] | None = None, *,
                 codepage: text_encoder.CodePage = text_encoder.DEFAULT) -> Compiler:
   """!Compiles a manifest file (the editor's entry point).
   @param path      manifest path
   @param sources   {relative path: text} overriding files, or None
   @param codepage  code page of the inline texts
   @return the Compiler (diags, result)
   """
   path = Path(path)
   manifest = json.loads(path.read_text(encoding="utf-8"))
   assets = maps = None
   asset_deps: list[str] = []
   if manifest.get("assets"):
      try:
         assets = load_asset_keys(path.parent / manifest["assets"], asset_deps)
      except (OSError, ValueError) as e:
         c = Compiler(manifest, path.parent, sources, codepage=codepage)
         c.err(f"cannot read assets manifest: {e}")
         return c
   if manifest.get("world"):
      try:
         maps = load_map_names(path.parent / manifest["world"])
      except (OSError, ValueError) as e:
         c = Compiler(manifest, path.parent, sources, codepage=codepage)
         c.err(f"cannot read world manifest: {e}")
         return c
   comp = Compiler(manifest, path.parent, sources, assets, maps, codepage=codepage)
   read = []
   if manifest.get("world"):
      try:
         comp.world_uses, read = load_world_uses(path.parent / manifest["world"])
         comp.vehicle_names = load_vehicle_names(path.parent / manifest["world"])
      except (OSError, ValueError):
         pass   # reported above
      try:
         comp.world_field = load_world_field(path.parent / manifest["world"])
      except (OSError, ValueError):
         pass   # reported above
      except ScriptError as e:
         comp.err(e.message)
   comp.run()
   for key in ("assets", "world"):
      if manifest.get(key):
         comp.deps.append(str(path.parent / manifest[key]))
   comp.deps.extend(read)
   comp.deps.extend(asset_deps)
   return comp


def main(argv: list[str]) -> int:
   """!Command-line entry point; see the module docstring.
   @param argv  sys.argv
   @return process exit status
   """
   args = argv[1:]
   try:
      codepage = text_encoder.option(args)
   except text_encoder.TextError as e:
      print(f"error: {e}", file=sys.stderr)
      return 1
   if len(args) < MIN_ARGUMENTS:
      print(__doc__)
      return EXIT_USAGE

   def option(name: str) -> str | None:
      """!Value of a --name option.
      @param name  option
      @return its value, or None
      """
      if name in args:
         i = args.index(name)
         return args[i + 1]
      return None

   manifest, out = args[0], args[1]
   try:
      comp = compile_file(manifest, codepage=codepage)
   except (OSError, ValueError) as e:
      print(f"{manifest}: error: {e}", file=sys.stderr)
      return 1
   for d in comp.diags:
      print(f"{Path(manifest).parent / d.file if d.file else manifest}"
            f"{':' + str(d.line) + ':' + str(d.col) if d.line else ''}: {d.severity}: {d.message}",
            file=sys.stderr)
   if comp.errors():
      return 1
   name = Path(manifest).name
   dst = Path(out)
   dst.parent.mkdir(parents=True, exist_ok=True)
   dst.write_text(comp.emit_cpp(name, "--debug-names" in args), encoding="utf-8")
   ids = option("--ids")
   if ids:
      Path(ids).parent.mkdir(parents=True, exist_ok=True)
      Path(ids).write_text(comp.emit_ids(name), encoding="utf-8")
   depfile = option("--depfile")
   if depfile:
      Path(depfile).write_text(f"{out}: " + " ".join(str(p).replace(" ", "\\ ") for p in comp.deps) + "\n",
                               encoding="utf-8")
   r = comp.result
   print(f"{manifest}: {r['script_count']} scripts, {len(r['flag_names'])} flags, {len(r['vars'])} variables, "
         f"{len(r['code'])} B code, {comp.save_bytes()} B save")
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv))
