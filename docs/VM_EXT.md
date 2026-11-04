# VM extension manifests (capsule.vm-ext/1)

D-39: the engine's virtual machines express a game with generic operations. A game's own opcodes are declared in
**extension manifests**, one format for every VM. An opcode is either an **alias** (data only: a short fixed
sequence of generic steps with the opcode's operand layout) or a **native** (a named C++ function in the game's
tier, with the reason no generic step works). The tool is `tools/vm_ext.py`; the C++ vocabulary is
`include/capsule/core/vm_ext.h`.

## 1. What is generated from a manifest

| From the manifest | Written by | Used by |
|---|---|---|
| the VM's dispatch: ROM tables of alias steps, one small function per alias, the command table | `vm_ext.py dispatch` (CMake: `capsule_vm_dispatch()`) | the game's build |
| the battle script banks' command list (ids and operand kinds) | `script_bank.py` (a source's `"manifests"`) | bank sources, converters |
| the animation banks' game opcodes (names and signatures) | `anim_bank.py` (a `.canim`'s `.manifest` line, `declare_manifests()`) | `.canim` files, converters |
| usage records (`capsule.vm-usage/1`: how often each bank emits each op) | `script_bank.py asm --usage`, `anim_bank.py assemble --usage` | the dispatch's exclusions |
| exclusion records (`capsule.vm-exclusions/1`: ops no bank uses, never compiled) | `vm_ext.py dispatch --usage ... --exclusions` | D-36 §5 reports |
| the extension report (each op's kind, reason, owner, size on the GBA) | `vm_ext.py report [--sizes nm.txt]` | the studio's Extensions view |

The checks (`vm_ext.py check`, CMake: `capsule_vm_ext_check()`):

- errors: a schema error; a native without a reason; two manifests of one VM claiming the same opcodes; a native
  whose function the sources do not define; a function of the manifest's `native_prefix` with no manifest entry;
  an op a bank emits (usage) that no manifest declares;
- warnings (errors with `--warnings-as-errors`): a native whose alias candidate has `"parity": "proven"`.

Aliases cost ROM only: steps are `const` tables and each alias function is a two-line call of the VM's
`run_alias()`. Nothing a game keeps in RAM changes.

## 2. The format

```json
{
  "schema": "capsule.vm-ext/1",
  "vm": "battle",
  "name": "sandbox",
  "owner": "sandbox",
  "tier": 0,
  "space": {"first": 0, "last": 7},
  "refs": {"lap": {"kind": "variable", "id": 0}},
  "dispatch": {"form": "fragment", "symbol": "kTable", "count": 8, "resolver": "resolve_ref",
               "native_prefix": "cmd_"},
  "ops": [
    {"id": 0, "name": "goto", "kind": "alias", "operands": [{"name": "target", "type": "code"}],
     "expands": [{"op": "jump", "args": {"target": "target"}}]},
    {"id": 1, "name": "slide", "kind": "native", "symbol": "cmd_slide", "waits": true,
     "operands": [{"name": "square", "type": "u8"}, {"name": "pixels", "type": "s8"}],
     "reason": "Sends a controller command to the square's endpoint."}
  ]
}
```

- `vm`: `script`, `battle`, `anim`, `effect`, `seq` or `screen-task`. Dispatch is generated for `battle` and `anim`
  today; the others load and check, and their dispatch comes with D-36 §6.
- `space`: the opcode range the manifest claims. Manifests of one VM must not overlap. Animation opcodes below
  0x40 are the engine's generic commands: a game may replace one only with `"override": true`.
- `refs`: named references alias steps use as constants. Battle: `{kind, id, offset}` (include/capsule/battle/vm.h
  references); animation: the game's own numbers (its resolver's).
- `dispatch`: `form` `source` (a translation unit defining `extern const` `symbol`, natives visible by name) or
  `fragment` (text the game's source includes where its natives are visible, defining a `constexpr` `symbol`);
  `namespace`, `includes` (source form), `count` (battle: the table's entries), `resolver` (the function that
  resolves Ref arguments, or null), `unknown` and `trace` (battle: CommandTable's), `native_prefix` (the checks'
  "every implementation has an entry"), `helpers` (functions of that prefix that are not ops).
- Per op: `id`, `name`, `operands` (types below, each with a `name` alias steps refer to), `kind`, `waits` (it may
  block on controllers or frames), `owner`, `tier`, `glitch` (D-38 hook ids), and
  - alias: `expands`, its steps;
  - native: `symbol` (the C++ function), `reason` (required), optionally `candidate` (`expands` + `parity`
    `pending` / `proven`: an alias that may replace it once parity is proven);
  - `keep`: never excluded by usage; `sub_ops`: a nested table (`selector`: the operand that picks the sub-op,
    `ops`: entries of the same format), for a command whose first operand picks one of many sub-commands.

Operand types: `u8`, `s8`, `u16`, `s16`, `u32`, `code` (a jump target), `ref` (a reference to game state), `battler`
(u8), `string` (u16); animation only: `template`, `task`, `sound_task`, `callback`, `data` (symbols), `args` (an
argument list; not in aliases).

## 3. Generic steps

Each step names its arguments: an operand of the op (by name), `{"const": n}` (compares by name: `equal`,
`not_equal`, `greater`, `less`, `common_bits`, `no_common_bits`), or `{"ref": name}` / `{"ref": {kind, id, offset}}`.

| Step | Arguments | Extra | Battle | Anim | Does |
|---|---|---|---|---|---|
| `nop` | | | yes | yes | falls through |
| `jump` | target | | yes | yes | goes to target |
| `jump_if` | compare, ref, value, target | width | yes | yes | goes to target when `*ref compare value` |
| `jump_if_bytes` | a, b, size, target | when | yes | yes | goes to target when the bytes are (not) equal |
| `call` | target | | yes | | pushes the next command, goes to target |
| `return` | | | yes | | pops and goes back |
| `end` | | | yes | | ends the script |
| `yield` | | | yes | yes | the run loop returns after this command |
| `pause` | frames | | yes | | waits frames (the machine's counter) |
| `pause_if_set` | flag, frames | | yes | | flag clear: falls through; set: waits frames, clears it |
| `wait_until` | compare, ref, value | width | yes | yes | waits a frame until `*ref compare value` |
| `hold` | | | yes | yes | stays on the command without waiting |
| `modify` | ref, value | width, alu | yes | yes | `*ref = *ref alu value` (`set`, `add`, `sub`, `or`, `bic`) |
| `copy` | dest, src, size | | yes | yes | copies size bytes |

Steps run in order on the command's position. A step that moves or waits ends the command for now; when every step
falls through, the VM moves past the command. An alias stops where its native would: operands past the code,
targets outside it (animation: the program counter left after the bad operand, as the operand readers leave it)
and unresolved references fault the VM (battle: `Fault::OutOfCode`, `BadRef`, `BadStep`; animation:
`PlayerError::BadOperand`, `BadOpcode`).

## 4. Proving an alias

An alias replaces a native only once it produces the same frames, RNG draws and results. The engine's tests do
this per step (tests/battle_vm_alias_tests.cpp, tests/anim_alias_tests.cpp: the same bank on aliases and on natives
of the same layouts, compared after every step or frame); a game proves its conversions the same way and with its
own parity scenarios, then removes the native and lets the check confirm no stray implementation is left.

## 5. The other VMs (later: D-36 §6)

The schema already names every VM (`script`, `effect`, `seq`, `screen-task`); `vm_ext.py` loads and checks their
manifests and refuses to generate their dispatch until each VM has its generic step runner. What each needs:

- **Field script VM** (`script/`). Its own opcodes are a manifest since D-36 package 2 step A:
  `src/script/script_vm.json` declares the 87 opcodes as `"kind": "generic"` ops (a kind only the script VM's
  manifests may use) in their bytecode numbering, with each op's operand bytes and action, the kind enums their
  operands select (each value with its script word) and the operand constants. `tools/script_vm.py header` writes
  `include/capsule/script/ops.h` from it (checked up to date by `script_vm_tests`), and `script_compiler.py` takes
  its opcode list, words and constants from it: there is no other copy. Since step B the ops of the engine's
  gameplay modules (`"module"`: party roles, the bag, the boxes, growth, combination, the nursery) name a C++
  `"handler"` and their waits a poll (`"polls"`): they run through a dispatch table (`src/script/script_dispatch.h`)
  that each game writes from its scripts' usage record with `tools/script_vm.py dispatch --usage <scripts.json>`,
  listing only the ops its scripts use, so a module none of them reaches is not linked.
  `src/script/script_dispatch_all.cpp` is the default table with every op. Its natives go through one `game::native(id, args, argc, result)` switch with
  evaluated `int32_t` arguments, not operand bytes. A manifest entry per native id (name, argument count, `waits`,
  `reason`) can replace the switch with a generated table of function pointers; aliases there are natives whose
  body is a variable read / write, a flag test or a plain wait, run as the VM's own expressions. Needs: a
  per-native argument layout in the manifest (argument names and kinds instead of operand bytes), the generated
  table in place of `game::native()`, and `script_compiler.py` reading native ids and arities from the manifest.
- **Effect VM** (`battle/effect.h`, D-23): about 100 fixed ops and no extension point. Needs: an opcode space
  for games (as the animation player's 0x40-0xFF), `run_alias()` over its context (its references are battle
  fields, not banked variables), and `effect_compiler.py` taking op layouts from the manifest.
- **Sequence VM** (`seq/`): presenter hooks and `game::` conditions, again functions, not opcodes. Needs: the
  hooks named in a manifest (`kind: native` with a reason) so the generated presenter table replaces the
  hand-written one, and `sequence_compiler.py` reading the hook names from it.
- **Screen tasks** (the animation player's visual and sound tasks, sprite callbacks): bound by symbol name, not
  opcode. Needs: symbols as manifest entries (kind `native`, the task's function, its reason), so the generated
  symbol tables come from the manifest too and the checks cover them.
