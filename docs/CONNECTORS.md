# Connectors

D-10 section 4: a **connector** is the part of a plugin that adds a game's own data to the engine's manifests and
turns it into code or data the game's build links. The connector declares **blocks** (typed fields) at **attach
points**, checks them with hooks, and writes outputs. The engine's tool is `tools/connectors.py`; the studio reads
the same declarations to build its inspector (the toolkit's Plugin data panel) and runs the same checks live.

Connectors are pure Python and use the standard library only: no native modules, no non-`py3-none-any` wheels,
no `ctypes`, `cffi` or Qt imports. A plugin that breaks this is refused before its code is imported.

## 1. A plugin with a connector

```json
{
  "schema": "capsule.plugin/1",
  "id": "sample.party_moods",
  "version": "2026.11.12.1080",
  "api": 3,
  "title": "Party moods (sample connector)",
  "connector": {"module": "party_moods.connector"}
}
```

`connector.module` names a module inside the plugin's folder. Its `register(c)` function declares everything; the
connector API is 3. A game lists its plugin folders in `reference/project.json`: `"plugins": ["plugins/party_moods"]`
(relative to the game's folder); plugin ids are unique and run in id order.

```python
def register(c):
   c.attach("party", c.block(
      mood=c.choice("calm", "angry", "cheerful", default="calm", title="Mood"),
      bonus=c.integer(0, 20, default=0, title="Mood bonus"),
      rival=c.ref("party", default=None, title="Rival")))
   c.on_validate(validate)
   c.on_emit(emit)
```

The sample is `samples/plugins/party_moods`.

## 2. Attach points

| Point | Where the block is |
|---|---|
| `project` | `reference/project.json`: `"ext"` |
| `world` | `world.json`: `"ext"` |
| `map` | a map file: `"ext"` |
| `cell` | a map file: `"cell_ext": {"PLUGIN_ID": [{"at": [x, y], ...}]}` |
| `layer` | a map file: an entry of `"layers"`: `"ext"` |
| `species`, `move`, `item`, `party`, `presenter` | `battle.json`: an entry of that list: `"ext"` |
| `asset` | `assets.json`: an entry of `"assets"`: `"ext"` |

Every block is keyed by the plugin's id: `"ext": {"sample.party_moods": {"mood": "angry"}}`. The compilers take
the blocks off at these points only, so an `"ext"` anywhere else is still an unknown key. A cell's `at` must be
inside one of the map's chunks (negative cells are allowed); a cell is named `MAP@x,y`, a layer `MAP/BG`.
`connectors.py attach-points` lists them.

## 3. Fields

| Helper | Value in the manifest | Value the hooks see |
|---|---|---|
| `c.integer(min, max)` | an integer in range | the integer |
| `c.boolean()` | `true` / `false` | the boolean |
| `c.choice(*names)` | one of the names | its index |
| `c.text(max_length)` | a string | the string |
| `c.ref(kind)` | a name: species, move, item, party, presenter, map or asset key | its number, as the compilers number it |
| `c.list_of(field, max_items)` | a list | the resolved list |
| `c.block(**fields)` | an object; unknown keys are errors | the resolved object, defaults filled in |

Every helper takes `default`, `title` and `help`; a field without a default is required. The numbers a
reference resolves to are the compilers' own: map ids follow the world's `"maps"` order, battle lists follow
`battle_ids.h` (items start at 1), assets are Asset Key v1.

## 4. Hooks and outputs

A build runs in stages, and schema errors stop it before references resolve:

1. **collect**: every block at every attach point, checked against its schema;
2. **resolve**: references become numbers; then each `on_resolve(record, ctx)` hook;
3. **validate**: each `on_validate(record, ctx)`; `ctx.error()` fails the build, `ctx.warning()` does not;
4. **emit**: each `on_emit(ctx)`; `ctx.write(name, content)` writes an output.

A record carries `attach`, `name`, `where`, `number` and `values`. The context gives `records(attach)`,
`names(kind)` and `resolve(kind, name)`. A hook that raises becomes an error naming the plugin and the stage.

Outputs end in `.h`, `.hpp`, `.inc`, `.cpp`, `.bin` or `.txt`, are written once, and stay inside the plugin's
folder: `ext/PLUGIN_ID/NAME` in the game's generated folder, which is on the include path. Every `.cpp` is compiled
through one generated `ext/connectors.cpp`. Outputs are written only when they change, and outputs a plugin no
longer writes are removed.

Transform hooks (changing a manifest before it compiles) are not supported yet.

## 5. The build and the command line

`capsule_game()` runs `connectors.py build` before the other compilers, with a depfile, so a game rebuilds its
connector outputs when a manifest, a plugin or the tool changes.

```sh
python3 tools/connectors.py build GAME/reference/project.json --out-dir build/gen/ext --depfile build/gen/ext.d
python3 tools/connectors.py check GAME/reference/project.json    # problems only, nothing written
python3 tools/connectors.py attach-points
```

## 6. Tools that edit blocks

The Studio's Plugin data panel and `capsule-studio ext` change blocks through the same module, so a tool and the
build agree on where a block lives:

| Function | Does |
|---|---|
| `targets(game, point)` | every record a block can go on at a point (`Target`: attach point, name, file, path to the object holding `"ext"`); for `cell`, one per map |
| `find_target(game, point, name)` | one record (`MAP@x,y` for a cell) |
| `get_block(data, target, plugin_id)` | the block as written, or `None` |
| `set_block(data, target, plugin_id, block)` | sets or removes (`None`) it in place; an `"ext"` or cell list left empty is removed |
| `cells_of(data, plugin_id)` | the cells of a map that carry the plugin's block |

Battle and asset records are the entries of `battle.json` and `assets.json` themselves, the files a tool edits.
`Game(project, sources)` reads unsaved data in place of files, so a tool checks what it shows. A field's
`describe()` (and `ConnectorSpec.describe()`) is the plain JSON a tool builds its form from.

The sample plugin also has a Studio extension (`party_moods/studio.py`, plugin.json `"extension"`): a CSV file
format for the opponents' moods and a preview. The build never imports it.

Tests: `tests/test_connectors.py` (CTest `connectors_tests`).
