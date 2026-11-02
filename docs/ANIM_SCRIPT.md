# Animation script banks and the player shell (BF, D-38)

The engine's animation-script player (`include/capsule/anim/player.h`) runs scripts stored in **banks**
(`include/capsule/anim/bank.h`). A script is a byte-coded list of commands: the engine's generic commands (waits,
jumps, calls, arguments, sprites, visual and sound tasks, graphics, sound, colour effects, layers) and the game's own
commands. A game supplies the tables the scripts refer to (sprite templates with their affine animations, visual
tasks, sound tasks, sprite callbacks, data), a command table (which function runs each opcode) and hooks (where a
sprite goes, how a sound plays, when a script may end).

`tools/anim_bank.py` writes and reads banks (a Python API for converters, an assembler for `.canim` text, a
disassembler); `capsule_anim_view` (cmake/parts/anim.cmake) disassembles a bank and plays the sandbox's scenes.

## 1. Bank layout

Every number is little-endian. Sections start on a multiple of 4 bytes; offsets count from the bank's first byte.

| Offset | Size | Header field |
| --- | --- | --- |
| 0 | u32 | magic `0x4D4E4143` (the bytes `C A N M`) |
| 4 | u16 | version (1) |
| 6 | u16 | flags: bit 0 `indexed`: every symbol carries its game table index |
| 8 | u16 | script count |
| 10 | u16 | symbol count |
| 12 | u16 | game opcode entries |
| 14 | u16 | reserved, 0 |
| 16 | u32 | offset of the script entries |
| 20 | u32 | offset of the symbols |
| 24 | u32 | offset of the opcode entries |
| 28 | u32 | offset of the code |
| 32 | u32 | bytes of code |
| 36 | u32 | offset of the strings |
| 40 | u32 | bytes of strings |

Header: 44 bytes.

| Section | Entry | Fields |
| --- | --- | --- |
| Scripts | 8 bytes | u32 code address of the script's first command; u32 name (string offset, `0xFFFFFFFF`: none) |
| Symbols | 8 bytes | u8 kind (0 template, 1 visual task, 2 sound task, 3 sprite callback, 4 data); u8 reserved 0; u16 index in the game's table of that kind (`0xFFFF`: unbound); u32 name |
| Opcodes | 12 bytes | u8 game opcode (0x40-0xFF); 3 reserved bytes 0; u32 name; u32 signature (string offset) |
| Code | - | commands only, one after the other: the opcode byte, then the operands its signature lists, no padding |
| Strings | - | NUL-terminated ASCII; a string offset counts from the section's first byte |

Code addresses (the `a` operand, script entries) are offsets from the code section's first byte and land on a
command's opcode byte. Banks are read in place (ROM on the GBA); `anim::open()` checks the header and tables,
`anim::check_code()` the commands.

### Symbols and binding

An operand that names a game object (`T K S C D`) holds a **symbol index** (u16, into the bank's symbol table).
The symbol's kind must match the letter. At run time the player turns a symbol into an index of the game's table:

- **Indexed banks** (flag bit 0, the GBA's way): the converter writes each symbol's game index; the game's tables
  are generated in the same order (the converter writes the name list the game's table generator reads). No RAM.
- **Bound banks**: `anim::bind()` looks every symbol up by name in the game's tables (`Tables::names`) and fills a
  map of `symbol count` u16s; `start()` takes the map. For host tools, tests and banks written without indices.

Names are the converter's (for a converted game: the source's symbol names). The engine never reads them except in
`bind()` and the tools.

## 2. Signatures

A signature is a string of operand letters, read left to right after the opcode byte.

| Letter | Operand | Bytes |
| --- | --- | --- |
| `b` | u8 | 1 |
| `c` | s8 | 1 |
| `h` | u16 | 2 |
| `s` | s16 | 2 |
| `w` | u32 | 4 |
| `a` | code address | 4 |
| `T` | symbol: sprite template | 2 |
| `K` | symbol: visual task | 2 |
| `S` | symbol: sound task | 2 |
| `C` | symbol: sprite callback | 2 |
| `D` | symbol: data | 2 |
| `v` | argument list: u8 count (at most 8), then count s16 | 1 + 2 x count |

The generic commands' signatures are fixed by the engine (`anim::op_signature()`); a game command's is declared in
the bank's opcode entries, so tools can read any bank without the game.

## 3. Generic commands (opcodes 0x00-0x13; 0x14-0x3F reserved for the engine)

| Op | Name | Signature | What it does |
| --- | --- | --- | --- |
| 0x00 | `end` | | While a visual or sound task runs: waits (`yield`). Then the game's `finish` hook (default yes); yes: the script stops. |
| 0x01 | `delay` | `b` | Wait mode for n frames (0 counts as -1). The next command runs n + 2 frames later. |
| 0x02 | `wait_visual` | | Visual count 0: next command (same frame). Else: `yield` and try again. |
| 0x03 | `wait_sound` | | Sound count 0 and the game's `sound_busy` hook false: next command. Else: `yield`. |
| 0x04 | `jump` | `a` | Goes to the address. |
| 0x05 | `call` | `a` | Pushes the next command's address (at most 4 nested) and goes to the address. |
| 0x06 | `return` | | Pops the address of the last `call`. |
| 0x07 | `set_arg` | `bs` | Argument n = value. |
| 0x08 | `jump_if_arg_eq` | `bsa` | Goes to the address when argument n equals the value. |
| 0x09 | `create_sprite` | `Tbv` | The arguments set (entries past the count keep their values), then the game's `create_sprite` hook makes the sprite (default: at 0, 0 with subpriority = the flags byte). A sprite made adds one visual. |
| 0x0A | `create_task` | `Kbv` | The arguments set, then a visual task with the priority, called once at once; adds one visual. |
| 0x0B | `create_sound_task` | `Sbv` | The same for a sound task; adds one sound task. |
| 0x0C | `load_gfx` | `h` | The game's `load_gfx` hook with the tag; then wait mode for one frame (the next command runs 3 frames later). |
| 0x0D | `unload_gfx` | `h` | The game's `unload_gfx` hook. |
| 0x0E | `play_se` | `hc` | The game's `play_se` hook: sound, pan (-64 left to 63 right). |
| 0x0F | `set_blend` | `bbbbb` | Colour effect: mode (0 off, 1 alpha, 2 brighten, 3 darken), first targets, second targets (layer masks: bits 0-3 BG0-3, 4 sprites, 5 backdrop), weights eva, evb. |
| 0x10 | `blend_off` | | Colour effect off, weights 0. |
| 0x11 | `bg_show` | `bb` | Shows (1) or hides (0) background layer n (`screen::layers::show()`). |
| 0x12 | `bg_scroll` | `bss` | Sets background layer n's scroll. |
| 0x13 | `nop` | | Nothing; the next command. |

A visual task or sprite a script made calls `anim::finish_task()` / `anim::finish_sprite()` when it is done (the
sprite's matrix is freed too); a sound task `anim::finish_sound_task()`. The counts are what `wait_visual`,
`wait_sound` and `end` wait for.

## 4. Game commands (opcodes 0x40-0xFF)

A game command is a function `void cmd(anim::Player& p)` in the game's command table at its opcode. It reads its
operands with `read_u8()` ... `read_args()` in its signature's order (or calls `jump()`), then does its work. It
must advance (read its operands) unless it waits (`yield()`, `wait_frames()`) or stops the player (`fail()`).
`read_symbol(p, kind)` returns the game table index of a symbol operand. A game may also put its own function at a
generic opcode (`anim::with(generic_table(), op, fn)`), for example an `end` with more conditions.

The bank declares each game opcode it uses with its name and signature (the `.opcode` line of a `.canim` file,
`BankWriter.opcode()` in Python).

## 5. Frames

`anim::frame(p)` runs once per frame (the game calls it from its main callback, where its scene runs).

- **Run mode**: commands run one after the other while `frames_to_wait` is 0 and the script is active. `yield()`
  sets `frames_to_wait` to 1 and the loop ends; the next frame runs the command at `pc` again (a command that waits
  does not advance) or the next one.
- **Wait mode** (`wait_frames(n)`: `delay`, `load_gfx`): each frame counts `frames_to_wait` down by one; on the frame
  it is 0 or less the player leaves wait mode and nothing else runs; commands run again the next frame.
- A frame that runs `kMaxCommandsPerFrame` (1024) commands stops the player with `Runaway`.

Worked example (frame 1 is the first `frame()` after `start()`):

```
f1:  create_sprite   (runs; visual 1)
     delay 2         (wait mode, frames_to_wait 2)
f2:  2 -> 1
f3:  1 -> 0
f4:  0: leave wait mode
f5:  wait_visual     (visual 1: yield)
...  (the sprite calls finish_sprite() on frame 9, during the sprite step after frame())
f10: wait_visual     (visual 0: next command, same frame)
     end             (nothing runs: the script stops; running() is false from now)
```

## 6. Writing banks

`tools/anim_bank.py` (Python 3.10, standard library):

- `BankWriter`: `symbol(kind, name, index=None)`, `opcode(op, name, signature)`, `script(name)`, `label(name)`,
  `emit(command, *operands)` (a generic command's name or a game opcode's name; addresses as label names; argument
  lists as lists), `build(indexed=False) -> bytes`.
- `read_bank(data) -> Bank` and `disassemble(bank) -> str`.
- The command line: `anim_bank.py assemble IN.canim OUT.bin [--indexed]`, `anim_bank.py disasm BANK.bin`,
  `anim_bank.py check BANK.bin`.

A `.canim` file:

```
.opcode 0x40 shake "bb"          # a game command: name, signature
.symbol template square 0        # optional: a symbol's game index (needed with --indexed)
.script spin_and_fade
   load_gfx 1
   create_sprite T:square 3 [120, 80, 4]
   create_task K:flash 2 [1]
   shake 2 8
loop:
   delay 4
   jump_if_arg_eq 0 1 done
   set_arg 0 1
   jump loop
done:
   wait_visual
   end
```

Operands: numbers (decimal, `0x` hex, negative), `T:name` / `K:` / `S:` / `C:` / `D:` symbols, label names for
addresses, `[a, b, ...]` argument lists. `#` starts a comment.

## 7. Converting another engine's scripts

A converter maps each source command onto a generic command (when its behaviour is the generic one) or onto a game
opcode with the source's operand layout as its signature, and writes the source's pointers as symbols (sprite
templates, task functions) or code addresses (jumps, calls). The behaviour of the game commands is the game's code;
the player only runs it. Sprites made by scripts animate in the screen framework (`capsule/screen/sprites.h`,
frame animations; `capsule/screen/affine.h`, affine animations), tasks run in its task list
(`capsule/screen/tasks.h`), per-scanline effects come from `capsule/screen/scanline.h`, palette blends from
`capsule/screen/blend.h`.
