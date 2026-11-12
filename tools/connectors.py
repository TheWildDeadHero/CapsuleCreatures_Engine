#!/usr/bin/env python3
"""!Connectors (D-10 §4): project-specific data in a game's manifests, checked and compiled by the build.

    connectors.py build <reference/project.json> --out-dir <generated/GAME/ext> [--depfile <ext.d>]
    connectors.py check <reference/project.json>          the checks only (no files written)
    connectors.py attach-points                            the attach points and where their blocks live

A game lists its plugins in reference/project.json ("plugins": ["plugins/party_moods"], folders relative to the game
folder). A plugin is a folder with a plugin.json (capsule.plugin/1); its connector is the module named by
"connector": {"module": "party_moods.connector"}, which defines register(c):

    def register(c):
       c.attach("party", c.block(mood=c.choice("calm", "angry", "cheerful", default="calm"),
                                 bonus=c.integer(0, 20, default=0),
                                 rival=c.ref("party", default=None)))
       c.on_validate(validate)     # validate(record, ctx): ctx.error(record, text) / ctx.warning(record, text)
       c.on_resolve(resolve)       # resolve(record, ctx): adds to record.values after the references resolve
       c.on_emit(emit)             # emit(ctx): ctx.write("party_moods.h", text) for each output

Project data then lives in namespaced blocks at the attach points: "ext": {"sample.party_moods": {"mood": "angry"}}.

Attach points (ATTACH_POINTS): project (project.json), world (world.json), map (a map file), cell (a map file's
"cell_ext": {plugin: [{"at": [x, y], ...}]}), layer (an entry of a map's "layers"), species, move, item, party,
presenter (entries of the battle manifest's lists) and asset (an entry of the asset manifest's "assets"). Each
compiler takes the blocks off its records before it checks their keys (without_ext()), so an "ext" anywhere else is
still an unknown key there; this tool refuses a block whose plugin is not loaded, or does not attach there.

The build, in order: the plugins load (pure Python only: no native files or non-py3-none-any wheels, no ctypes /
cffi and no Qt in connector code); every block is checked against its schema (an error stops the build before any
hook runs); references resolve to the numbers the compilers give (species, move, item, party and presenter: their
place in the battle lists, as battle_ids.h; map: its place in the world's maps, as world_ids.h; asset: its Asset
Key); then each plugin's resolve, validate and emit hooks run. Outputs go to OUT_DIR/PLUGIN_ID/; OUT_DIR/
connectors.cpp includes every C++ source the plugins wrote, so the game compiles one known file. Files are written
only when their content changes, and files a plugin no longer writes are removed. Hooks get no clock, no network and
no files outside what ctx gives them, so the same sources give the same outputs.

Not built yet: the optional transform(doc, ctx) hook (a plugin that declares it is refused with a message).
"""
from __future__ import annotations

import ast
import contextlib
import copy
import importlib
import importlib.util
import json
import re
import sys
from collections.abc import Callable, Iterator, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

## A JSON value.
Json = Any

## Plugin manifest schema.
PLUGIN_SCHEMA = "capsule.plugin/1"
## Project manifest schema.
PROJECT_SCHEMA = "capsule.project/1"
## The plugin API numbers whose connectors this host runs (connectors arrived with plugin API 3).
CONNECTOR_APIS = (3,)
## The key of a record's namespaced blocks.
EXT_KEY = "ext"
## The key of a map file's per-cell blocks.
CELL_EXT_KEY = "cell_ext"
## The key of a cell block's position.
CELL_AT_KEY = "at"
## Plugin ids: dotted lower-case words.
PLUGIN_ID = re.compile(r"[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+")
## Names a plugin may give its outputs: folders and files of letters, digits, "_", "-" and ".".
OUTPUT_NAME = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]*(/[A-Za-z0-9_][A-Za-z0-9_.-]*)*")
## Output suffixes, and whether the game compiles the file (connectors.cpp includes it).
OUTPUT_SUFFIXES = {".h": False, ".hpp": False, ".inc": False, ".cpp": True, ".bin": False, ".txt": False}
## The file that includes every compiled output.
AGGREGATE_NAME = "connectors.cpp"
## Suffixes of native code a pure-Python plugin may not carry.
NATIVE_SUFFIXES = {".so", ".pyd", ".dll", ".dylib"}
## The wheel tag a vendored package must have.
PURE_WHEEL = "py3-none-any"
## Modules connector code must not import (no native bindings; builds run headless).
REFUSED_IMPORTS = ("ctypes", "cffi", "PySide6", "shiboken6", "PyQt5", "PyQt6")
## Width of a map chunk in cells (map_compiler's CHUNK_CELLS): a cell block must lie in one of the map's chunks.
CHUNK_CELLS = 8
## Numbers in a cell position.
CELL_AT_LENGTH = 2
## Index of the first item (battle_compiler: item 0 is "no item").
ITEM_FIRST = 1


class ConnectorError(Exception):
   """!A plugin, a block or a hook is wrong; the message names the file, the record and the plugin."""


# ---- attach points -------------------------------------------------------------------------------------------------

@dataclass(frozen=True)
class AttachPoint:
   """!Where project data may attach."""

   ## Name (what c.attach() takes).
   name: str
   ## Where the blocks live, for messages and the attach-points listing.
   where: str
   ## The reference kind its records resolve as (their numeric id), or "" when the record has none.
   ref_kind: str = ""


## Every attach point.
ATTACH_POINTS = {
   "project": AttachPoint("project", "reference/project.json: \"ext\""),
   "world": AttachPoint("world", "world.json: \"ext\""),
   "map": AttachPoint("map", "a map file: \"ext\"", "map"),
   "cell": AttachPoint("cell", "a map file: \"cell_ext\": {plugin: [{\"at\": [x, y], ...}]}"),
   "layer": AttachPoint("layer", "a map file: an entry of \"layers\": \"ext\""),
   "species": AttachPoint("species", "battle.json: an entry of \"species\": \"ext\"", "species"),
   "move": AttachPoint("move", "battle.json: an entry of \"moves\": \"ext\"", "move"),
   "item": AttachPoint("item", "battle.json: an entry of \"items\": \"ext\"", "item"),
   "party": AttachPoint("party", "battle.json: an entry of \"parties\" (an opponent's battle): \"ext\"", "party"),
   "presenter": AttachPoint("presenter", "battle.json: an entry of \"presenters\": \"ext\"", "presenter"),
   "asset": AttachPoint("asset", "assets.json: an entry of \"assets\": \"ext\"", "asset"),
}
## Battle attach points and the battle list each one's records are in.
BATTLE_LISTS = {"species": "species", "move": "moves", "item": "items", "party": "parties", "presenter": "presenters"}
## What a reference may name, and how (ctx.resolve()).
REF_KINDS = ("species", "move", "item", "party", "presenter", "map", "asset")


def without_ext(data: Json, kind: str) -> Json:
   """!A manifest with the connector blocks at its attach points taken off (each compiler calls it before checking
   keys, so an "ext" anywhere else stays an unknown key). Returns a copy; the input is not changed.
   @param data  the manifest (stripped of editor data or not)
   @param kind  "world", "map", "battle" or "assets"
   @return the manifest without them
   """
   if not isinstance(data, dict):
      return data
   out = dict(data)
   if kind in ("world", "project"):
      out.pop(EXT_KEY, None)
   elif kind == "map":
      out.pop(EXT_KEY, None)
      out.pop(CELL_EXT_KEY, None)
      if isinstance(out.get("layers"), list):
         out["layers"] = [_entry_without(layer) for layer in out["layers"]]
   elif kind == "battle":
      for key in BATTLE_LISTS.values():
         if isinstance(out.get(key), list):
            out[key] = [_entry_without(e) for e in out[key]]
   elif kind == "assets":
      if isinstance(out.get("assets"), list):
         out["assets"] = [_entry_without(e) for e in out["assets"]]
   return out


def _entry_without(entry: Json) -> Json:
   """!A list entry without its "ext" block.
   @param entry  the entry
   @return a copy without it (anything else unchanged)
   """
   if isinstance(entry, dict) and EXT_KEY in entry:
      return {k: v for k, v in entry.items() if k != EXT_KEY}
   return entry


# ---- schemas -------------------------------------------------------------------------------------------------------

## The default of a field that must be given.
REQUIRED = object()


class Field:
   """!One value in a block: what it may be, and what the Studio's inspector shows for it."""

   ## Type name (describe()).
   type_name = ""

   def __init__(self, default: Json = REQUIRED, title: str = "", help: str = "") -> None:
      """!Creates the field.
      @param default  its value when the block leaves it out (REQUIRED: it must be given)
      @param title    shown in the inspector (default: the key)
      @param help     the inspector's tooltip
      """
      ## Value when left out, or REQUIRED.
      self.default = default
      ## Inspector label.
      self.title = title
      ## Inspector tooltip.
      self.help = help

   def check(self, value: Json, where: str) -> list[str]:
      """!Problems with a value.
      @param value  the value
      @param where  its location
      @return messages (empty: it fits)
      """
      return []

   def resolve(self, value: Json, ctx: Context, where: str) -> Json:
      """!The value with its references resolved to numbers.
      @param value  a checked value
      @param ctx    the build context
      @param where  its location
      @return the value
      """
      return value

   def describe(self) -> dict[str, Json]:
      """!The field for the Studio's inspector (plain JSON).
      @return a dict
      """
      out: dict[str, Json] = {"type": self.type_name}
      if self.default is not REQUIRED:
         out["default"] = copy.deepcopy(self.default)
      else:
         out["required"] = True
      if self.title:
         out["title"] = self.title
      if self.help:
         out["help"] = self.help
      return out


class Integer(Field):
   """!A whole number in a range."""

   ## Type name.
   type_name = "integer"

   def __init__(self, minimum: int, maximum: int, default: Json = REQUIRED, title: str = "", help: str = "") -> None:
      """!Creates the field.
      @param minimum  smallest value
      @param maximum  largest value
      @param default  value when left out, or REQUIRED
      @param title    inspector label
      @param help     inspector tooltip
      """
      super().__init__(default, title, help)
      if minimum > maximum:
         raise ConnectorError(f"integer({minimum}, {maximum}): the smallest value is above the largest")
      ## Smallest value.
      self.minimum = minimum
      ## Largest value.
      self.maximum = maximum

   def check(self, value: Json, where: str) -> list[str]:
      """!A whole number within the range.
      @param value  the value
      @param where  its location
      @return messages
      """
      if isinstance(value, bool) or not isinstance(value, int):
         return [f"{where}: a whole number is needed, not {value!r}"]
      if not self.minimum <= value <= self.maximum:
         return [f"{where}: {value} is outside {self.minimum}..{self.maximum}"]
      return []

   def describe(self) -> dict[str, Json]:
      """!With its range.
      @return a dict
      """
      out = super().describe()
      low, high = self.minimum, self.maximum
      out.update({"minimum": low, "maximum": high})
      return out


class Boolean(Field):
   """!Yes or no."""

   ## Type name.
   type_name = "boolean"

   def check(self, value: Json, where: str) -> list[str]:
      """!true or false.
      @param value  the value
      @param where  its location
      @return messages
      """
      return [] if isinstance(value, bool) else [f"{where}: true or false is needed, not {value!r}"]


class Choice(Field):
   """!One of a list of names."""

   ## Type name.
   type_name = "choice"

   def __init__(self, choices: Sequence[str], default: Json = REQUIRED, title: str = "", help: str = "") -> None:
      """!Creates the field.
      @param choices  the names
      @param default  value when left out, or REQUIRED
      @param title    inspector label
      @param help     inspector tooltip
      """
      super().__init__(default, title, help)
      if not choices or len(set(choices)) != len(choices) or not all(isinstance(c, str) and c for c in choices):
         raise ConnectorError(f"choice{tuple(choices)!r}: needs distinct names")
      ## The names.
      self.choices = tuple(choices)

   def check(self, value: Json, where: str) -> list[str]:
      """!One of the names.
      @param value  the value
      @param where  its location
      @return messages
      """
      if value not in self.choices:
         return [f"{where}: one of {', '.join(self.choices)} is needed, not {value!r}"]
      return []

   def resolve(self, value: Json, ctx: Context, where: str) -> Json:
      """!A choice compiles to its index.
      @param value  the name
      @param ctx    the build context
      @param where  its location
      @return its index in the choices
      """
      return self.choices.index(value)

   def describe(self) -> dict[str, Json]:
      """!With its names.
      @return a dict
      """
      out = super().describe()
      choices = self.choices
      out["choices"] = list(choices)
      return out


class Text(Field):
   """!Free text, up to a length."""

   ## Type name.
   type_name = "text"

   def __init__(self, max_length: int, default: Json = REQUIRED, title: str = "", help: str = "") -> None:
      """!Creates the field.
      @param max_length  most characters
      @param default     value when left out, or REQUIRED
      @param title       inspector label
      @param help        inspector tooltip
      """
      super().__init__(default, title, help)
      ## Most characters.
      self.max_length = max_length

   def check(self, value: Json, where: str) -> list[str]:
      """!A string of at most max_length characters.
      @param value  the value
      @param where  its location
      @return messages
      """
      if not isinstance(value, str):
         return [f"{where}: text is needed, not {value!r}"]
      if len(value) > self.max_length:
         return [f"{where}: at most {self.max_length} characters ({len(value)} given)"]
      return []

   def describe(self) -> dict[str, Json]:
      """!With its length.
      @return a dict
      """
      out = super().describe()
      most = self.max_length
      out["max_length"] = most
      return out


class Ref(Field):
   """!A reference to another record by name (a species, a map, an asset ...); it compiles to that record's number
   (null stays null when the field allows it)."""

   ## Type name.
   type_name = "ref"

   def __init__(self, kind: str, default: Json = REQUIRED, title: str = "", help: str = "") -> None:
      """!Creates the field.
      @param kind     one of REF_KINDS
      @param default  value when left out (None: no reference), or REQUIRED
      @param title    inspector label
      @param help     inspector tooltip
      """
      super().__init__(default, title, help)
      if kind not in REF_KINDS:
         raise ConnectorError(f"ref({kind!r}): references name one of {', '.join(REF_KINDS)}")
      ## What it names.
      self.kind = kind

   def check(self, value: Json, where: str) -> list[str]:
      """!A name (or null when the field's default is null).
      @param value  the value
      @param where  its location
      @return messages
      """
      if value is None and self.default is None:
         return []
      if not isinstance(value, str) or not value:
         return [f"{where}: the name of a {self.kind} is needed, not {value!r}"]
      return []

   def resolve(self, value: Json, ctx: Context, where: str) -> Json:
      """!The named record's number.
      @param value  the name, or None
      @param ctx    the build context
      @param where  its location
      @return its number, or None
      """
      if value is None:
         return None
      return ctx.resolve(self.kind, value, where)

   def describe(self) -> dict[str, Json]:
      """!With what it names.
      @return a dict
      """
      out = super().describe()
      kind = self.kind
      out["kind"] = kind
      return out


class ListOf(Field):
   """!A list of values of one field."""

   ## Type name.
   type_name = "list"

   def __init__(self, item: Field, max_items: int, default: Json = REQUIRED, title: str = "",
                help: str = "") -> None:
      """!Creates the field.
      @param item       the field of each entry
      @param max_items  most entries
      @param default    value when left out, or REQUIRED
      @param title      inspector label
      @param help       inspector tooltip
      """
      super().__init__(default, title, help)
      ## Each entry's field.
      self.item = item
      ## Most entries.
      self.max_items = max_items

   def check(self, value: Json, where: str) -> list[str]:
      """!A list of at most max_items fitting entries.
      @param value  the value
      @param where  its location
      @return messages
      """
      if not isinstance(value, list):
         return [f"{where}: a list is needed, not {value!r}"]
      if len(value) > self.max_items:
         return [f"{where}: at most {self.max_items} entries ({len(value)} given)"]
      out: list[str] = []
      for n, entry in enumerate(value):
         out += self.item.check(entry, f"{where}[{n}]")
      return out

   def resolve(self, value: Json, ctx: Context, where: str) -> Json:
      """!Each entry resolved.
      @param value  the list
      @param ctx    the build context
      @param where  its location
      @return the resolved list
      """
      return [self.item.resolve(v, ctx, f"{where}[{n}]") for n, v in enumerate(value)]

   def describe(self) -> dict[str, Json]:
      """!With its entries' field.
      @return a dict
      """
      out = super().describe()
      most = self.max_items
      out.update({"item": self.item.describe(), "max_items": most})
      return out


class Block(Field):
   """!An object of named fields; every key must be one of them, and a field without a default must be given."""

   ## Type name.
   type_name = "block"

   def __init__(self, fields: Mapping[str, Field], default: Json = REQUIRED, title: str = "", help: str = "") -> None:
      """!Creates the field.
      @param fields   key -> field, in inspector order
      @param default  value when left out, or REQUIRED
      @param title    inspector label
      @param help     inspector tooltip
      """
      super().__init__(default, title, help)
      for key, f in fields.items():
         if not isinstance(f, Field):
            raise ConnectorError(f"block field {key!r} is not a field (use c.integer(), c.choice(), ...)")
      ## Key -> field.
      self.fields = dict(fields)

   def check(self, value: Json, where: str) -> list[str]:
      """!Known keys, each fitting; the fields without a default present.
      @param value  the value
      @param where  its location
      @return messages
      """
      if not isinstance(value, dict):
         return [f"{where}: an object is needed, not {value!r}"]
      unknown = sorted(set(value) - set(self.fields))
      if unknown:
         return [f"{where}: unknown key(s) {', '.join(unknown)} (it has {', '.join(self.fields) or 'none'})"]
      out: list[str] = []
      for key, f in self.fields.items():
         if key not in value:
            if f.default is REQUIRED:
               out.append(f"{where}: {key} is needed")
            continue
         out += f.check(value[key], f"{where}.{key}")
      return out

   def complete(self, value: Mapping[str, Json]) -> dict[str, Json]:
      """!A checked block with the defaults filled in.
      @param value  the block
      @return a copy with every field
      """
      out: dict[str, Json] = {}
      for key, f in self.fields.items():
         raw = value[key] if key in value else copy.deepcopy(f.default)
         out[key] = f.complete(raw) if isinstance(f, Block) and isinstance(raw, dict) else raw
      return out

   def resolve(self, value: Json, ctx: Context, where: str) -> Json:
      """!Every field resolved (defaults filled in).
      @param value  the block
      @param ctx    the build context
      @param where  its location
      @return the resolved block
      """
      full = self.complete(value)
      return {k: self.fields[k].resolve(v, ctx, f"{where}.{k}") for k, v in full.items()}

   def describe(self) -> dict[str, Json]:
      """!With its fields.
      @return a dict
      """
      out = super().describe()
      out["fields"] = {k: f.describe() for k, f in self.fields.items()}
      return out


# ---- what a connector registers ----------------------------------------------------------------------------------

## A validate or resolve hook: (record, ctx).
RecordHook = Callable[["Record", "Context"], None]
## An emit hook: (ctx).
EmitHook = Callable[["Context"], None]


class ConnectorSpec:
   """!What a connector's register(c) declares; also the schema helpers it calls (c.integer(), c.block(), ...)."""

   def __init__(self, plugin_id: str) -> None:
      """!An empty declaration.
      @param plugin_id  the plugin's id
      """
      ## The plugin's id.
      self.plugin_id = plugin_id
      ## Attach point -> its block's schema.
      self.attach_points = {}
      ## validate(record, ctx) hooks.
      self.validators = []
      ## resolve(record, ctx) hooks.
      self.resolvers = []
      ## emit(ctx) hooks.
      self.emitters = []

   # The schema helpers.
   def integer(self, minimum: int, maximum: int, default: Json = REQUIRED, title: str = "", help: str = "") -> Field:
      """!A whole number field.
      @param minimum  smallest value
      @param maximum  largest value
      @param default  value when left out
      @param title    inspector label
      @param help     inspector tooltip
      @return the field
      """
      return Integer(minimum, maximum, default, title, help)

   def boolean(self, default: Json = REQUIRED, title: str = "", help: str = "") -> Field:
      """!A yes / no field.
      @param default  value when left out
      @param title    inspector label
      @param help     inspector tooltip
      @return the field
      """
      return Boolean(default, title, help)

   def choice(self, *choices: str, default: Json = REQUIRED, title: str = "", help: str = "") -> Field:
      """!A field holding one of some names (compiled to the name's index).
      @param choices  the names
      @param default  value when left out
      @param title    inspector label
      @param help     inspector tooltip
      @return the field
      """
      return Choice(choices, default, title, help)

   def text(self, max_length: int, default: Json = REQUIRED, title: str = "", help: str = "") -> Field:
      """!A text field.
      @param max_length  most characters
      @param default     value when left out
      @param title       inspector label
      @param help        inspector tooltip
      @return the field
      """
      return Text(max_length, default, title, help)

   def ref(self, kind: str, default: Json = REQUIRED, title: str = "", help: str = "") -> Field:
      """!A reference to another record (compiled to its number).
      @param kind     one of REF_KINDS
      @param default  value when left out (None: no reference)
      @param title    inspector label
      @param help     inspector tooltip
      @return the field
      """
      return Ref(kind, default, title, help)

   def list_of(self, item: Field, max_items: int, default: Json = REQUIRED, title: str = "", help: str = "") -> Field:
      """!A list field.
      @param item       each entry's field
      @param max_items  most entries
      @param default    value when left out
      @param title      inspector label
      @param help       inspector tooltip
      @return the field
      """
      return ListOf(item, max_items, default, title, help)

   def block(self, title: str = "", help: str = "", **fields: Field) -> Block:
      """!An object of named fields (also what attach() takes).
      @param title   inspector label
      @param help    inspector tooltip
      @param fields  key=field, in inspector order
      @return the block
      """
      return Block(fields, REQUIRED, title, help)

   # The declarations.
   def attach(self, point: str, schema: Block) -> None:
      """!Declares a block at an attach point.
      @param point   one of ATTACH_POINTS
      @param schema  its schema (c.block(...))
      @return None
      @throws ConnectorError for an unknown attach point, one declared twice, or a schema that is not a block
      """
      if point not in ATTACH_POINTS:
         raise ConnectorError(f"plugin {self.plugin_id}: {point!r} is no attach point (they are "
                              f"{', '.join(ATTACH_POINTS)})")
      if point in self.attach_points:
         raise ConnectorError(f"plugin {self.plugin_id}: attaches at {point} twice")
      if not isinstance(schema, Block):
         raise ConnectorError(f"plugin {self.plugin_id}: attach({point!r}, ...) takes c.block(...)")
      self.attach_points[point] = schema

   def on_validate(self, hook: RecordHook) -> None:
      """!Adds a check run on every record carrying the block (after the schema and the references).
      @param hook  validate(record, ctx)
      @return None
      """
      self.validators.append(hook)

   def on_resolve(self, hook: RecordHook) -> None:
      """!Adds a step run on every record after its references resolve (it may add to record.values).
      @param hook  resolve(record, ctx)
      @return None
      """
      self.resolvers.append(hook)

   def on_emit(self, hook: EmitHook) -> None:
      """!Adds an output step (ctx.write() its files).
      @param hook  emit(ctx)
      @return None
      """
      self.emitters.append(hook)

   def on_transform(self, hook: Callable[..., Any]) -> None:
      """!Not built yet (D-10's optional pre-compile rewrite).
      @param hook  transform(doc, ctx)
      @return None
      @throws ConnectorError always
      """
      raise ConnectorError(f"plugin {self.plugin_id}: transform hooks are not supported yet")

   def describe(self) -> dict[str, Json]:
      """!The attach points and their schemas, for the Studio (plain JSON).
      @return {attach point: block description}
      """
      return {point: schema.describe() for point, schema in self.attach_points.items()}


@dataclass
class Record:
   """!A record carrying one plugin's block."""

   ## Its attach point.
   attach: str
   ## Its name: the map's, the species' ..., "project", "world", "MAP/BG" for a layer, "MAP@x,y" for a cell.
   name: str
   ## Where it is (file and position), for messages.
   where: str
   ## The block as written.
   block: dict[str, Json]
   ## Its number (the species' index, the map's, the asset's key ...), or None.
   number: int | None = None
   ## The block with the defaults filled in and the references resolved (resolve hooks may add to it).
   values: dict[str, Json] = field(default_factory=dict)


@dataclass
class Problem:
   """!A schema error or a hook's error or warning."""

   ## "error" or "warning".
   severity: str
   ## The plugin.
   plugin: str
   ## What is wrong, with where.
   message: str
   ## The record's attach point and name, or empty.
   record: tuple[str, str] = ("", "")

   def text(self) -> str:
      """!As the build prints it.
      @return the line
      """
      return f"{self.severity}: [{self.plugin}] {self.message}"


# ---- the game ------------------------------------------------------------------------------------------------------

class Game:
   """!A game's manifests, read once: where the blocks are, and the numbers references resolve to."""

   def __init__(self, project_path: str | Path, sources: Mapping[Path, Json] | None = None) -> None:
      """!Reads the project manifest.
      @param project_path  reference/project.json
      @param sources       {Path: data} replacing files (the Studio's unsaved documents), or None
      @throws ConnectorError when it cannot be read
      """
      ## The project manifest's path.
      self.project_path = Path(project_path).resolve()
      ## The game's folder (paths in project.json are relative to it).
      self.folder = self.project_path.parent.parent
      ## Replacements for files.
      self.sources = {Path(k).resolve(): v for k, v in (sources or {}).items()}
      ## Files read (the depfile).
      self.deps = [self.project_path]
      ## The project manifest.
      self.project = self.read(self.project_path)
      if not isinstance(self.project, dict) or self.project.get("schema") != PROJECT_SCHEMA:
         raise ConnectorError(f"{self.project_path}: schema must be {PROJECT_SCHEMA}")
      ## Cached lookups: kind -> {name: number}.
      self._numbers = {}
      ## Cached merged battle view, or None.
      self._battle = None

   def read(self, path: Path) -> Json:
      """!A JSON file (or its replacement), recorded as read.
      @param path  the file
      @return its data
      @throws ConnectorError when it cannot be read
      """
      path = path.resolve()
      if path in self.sources:
         return self.sources[path]
      if path not in self.deps:
         self.deps.append(path)
      try:
         return json.loads(path.read_text(encoding="utf-8"))
      except OSError as e:
         raise ConnectorError(f"{path}: cannot read ({e.strerror})") from None
      except json.JSONDecodeError as e:
         raise ConnectorError(f"{path}: invalid JSON ({e})") from None

   def path_of(self, key: str) -> Path | None:
      """!A manifest project.json names ("maps", "battle", "assets").
      @param key  its key
      @return its path, or None when the game has none
      """
      rel = self.project.get(key)
      return (self.folder / rel).resolve() if isinstance(rel, str) and rel else None

   def plugin_folders(self) -> list[Path]:
      """!The plugin folders project.json lists.
      @return folders
      @throws ConnectorError when "plugins" is malformed
      """
      raw = self.project.get("plugins", [])
      if not isinstance(raw, list) or not all(isinstance(p, str) for p in raw):
         raise ConnectorError(f"{self.project_path}: plugins lists plugin folders (relative to {self.folder})")
      return [(self.folder / p).resolve() for p in raw]

   # -- the blocks ----------------------------------------------------------------------------------------------

   def blocks(self) -> Iterator[tuple[str, str, str, int | None, Json]]:
      """!Every "ext" (and "cell_ext") block at an attach point, in manifest order.
      @return (attach point, record name, where, record number, the record's ext object)
      """
      yield "project", "project", str(self.project_path), None, self.project.get(EXT_KEY)
      world_path = self.path_of("maps")
      if world_path is not None:
         world = self.read(world_path)
         yield "world", "world", str(world_path), None, world.get(EXT_KEY) if isinstance(world, dict) else None
         maps = world.get("maps", {}) if isinstance(world, dict) else {}
         for number, (name, rel) in enumerate((maps or {}).items()):
            map_path = (world_path.parent / rel).resolve()
            data = self.read(map_path)
            if not isinstance(data, dict):
               continue
            yield "map", name, str(map_path), number, data.get(EXT_KEY)
            for n, layer in enumerate(data.get("layers") or []):
               if isinstance(layer, dict):
                  bg = layer.get("bg", n)
                  yield "layer", f"{name}/{bg}", f"{map_path}: layers[{n}]", None, layer.get(EXT_KEY)
            yield "cell", name, f"{map_path}: {CELL_EXT_KEY}", number, data.get(CELL_EXT_KEY)
      battle = self.battle()
      if battle is not None:
         where = str(self.path_of("battle"))
         for point, key in BATTLE_LISTS.items():
            first = ITEM_FIRST if point == "item" else 0
            for n, entry in enumerate(battle.get(key) or []):
               if isinstance(entry, dict):
                  yield point, str(entry.get("name")), f"{where}: {key}[{n}]", first + n, entry.get(EXT_KEY)
      assets_path = self.path_of("assets")
      if assets_path is not None:
         assets = self.read(assets_path)
         for n, entry in enumerate(assets.get("assets", []) if isinstance(assets, dict) else []):
            if isinstance(entry, dict):
               key = entry.get("key")
               yield "asset", str(key), f"{assets_path}: assets[{n}]", self._asset_number(key), entry.get(EXT_KEY)

   def battle(self) -> Json:
      """!The merged battle view (battle.json and its structure), or None.
      @return the data
      """
      if self._battle is None:
         path = self.path_of("battle")
         if path is None:
            return None
         from battlec import manifest as battle_manifest   # only games with battle data need it
         deps: list[Path] = []
         sources = self.sources or None
         try:
            self._battle, _structure = battle_manifest.read_battle(path, sources, deps, keep_ext=True)
         except battle_manifest.ManifestError as e:
            raise ConnectorError(f"{path}: {e}") from None
         self.deps += [d for d in deps if d not in self.deps]
      return self._battle

   def _asset_number(self, key: Json) -> int | None:
      """!An asset's key number (Asset Key v1).
      @param key  "namespace:Name"
      @return the number, or None when the key is malformed (the asset compiler reports it)
      """
      import sequence_compiler
      try:
         return sequence_compiler.asset_key(key)
      except sequence_compiler.ManifestError:
         return None

   # -- references ----------------------------------------------------------------------------------------------

   def numbers(self, kind: str) -> dict[str, int]:
      """!The records of a kind by name, with the numbers the compilers give them.
      @param kind  one of REF_KINDS except asset
      @return name -> number
      """
      if kind in self._numbers:
         return self._numbers[kind]
      out: dict[str, int] = {}
      if kind == "map":
         world_path = self.path_of("maps")
         world = self.read(world_path) if world_path is not None else {}
         out = {name: n for n, name in enumerate((world.get("maps") or {}) if isinstance(world, dict) else {})}
      elif kind in BATTLE_LISTS:
         battle = self.battle() or {}
         first = ITEM_FIRST if kind == "item" else 0
         for n, entry in enumerate(battle.get(BATTLE_LISTS[kind]) or []):
            if isinstance(entry, dict) and isinstance(entry.get("name"), str):
               out[entry["name"]] = first + n
      self._numbers[kind] = out
      return out

   def resolve(self, kind: str, name: str, where: str) -> int:
      """!A reference's number.
      @param kind   one of REF_KINDS
      @param name   the record's name ("namespace:Name" for an asset)
      @param where  location for messages
      @return the number
      @throws ConnectorError when there is no such record
      """
      if kind == "asset":
         import sequence_compiler
         try:
            return sequence_compiler.asset_key(name, where)
         except sequence_compiler.ManifestError as e:
            raise ConnectorError(str(e)) from None
      table = self.numbers(kind)
      if name not in table:
         raise ConnectorError(f"{where}: there is no {kind} {name!r}")
      return table[name]

   def map_chunks(self, map_name: str) -> set[tuple[int, int]] | None:
      """!A map's chunk positions (a cell block must lie in one).
      @param map_name  the map
      @return {(cx, cy)}, or None when the map cannot be read
      """
      world_path = self.path_of("maps")
      if world_path is None:
         return None
      world = self.read(world_path)
      rel = (world.get("maps") or {}).get(map_name) if isinstance(world, dict) else None
      if not isinstance(rel, str):
         return None
      data = self.read((world_path.parent / rel).resolve())
      out = set()
      for chunk in data.get("chunks", []) if isinstance(data, dict) else []:
         at = chunk.get("at") if isinstance(chunk, dict) else None
         if isinstance(at, list) and len(at) == CELL_AT_LENGTH:
            out.add((at[0], at[1]))
      return out


# ---- where a block can go (the Studio's edits) -------------------------------------------------------------------

@dataclass
class Target:
   """!A place a block can go: a record at an attach point, in the file that holds it."""

   ## Its attach point.
   attach: str
   ## The record's name, as Record.name gives it ("MAP@x,y" for a cell).
   name: str
   ## The file holding it.
   path: Path
   ## Keys and indexes from the file's top to the object holding "ext" (the map's object for a cell).
   pointer: tuple[str | int, ...]
   ## A cell's (x, y), or None.
   at: tuple[int, int] | None = None


def parse_cell(name: str) -> tuple[str, tuple[int, int]]:
   """!A cell record's name taken apart.
   @param name  "MAP@x,y"
   @return (map name, (x, y))
   @throws ConnectorError when it is not a cell's name
   """
   map_name, sep, at = name.partition("@")
   parts = at.split(",")
   try:
      if not sep or not map_name or len(parts) != CELL_AT_LENGTH:
         raise ValueError(name)
      return map_name, (int(parts[0]), int(parts[1]))
   except ValueError:
      raise ConnectorError(f"a cell is named MAP@x,y, not {name!r}") from None


def targets(game: Game, attach: str) -> list[Target]:
   """!Every record a block can attach to at a point, in manifest order. Battle and asset records are the entries of
   battle.json and assets.json themselves (not of the files they include), since those are the files edited.
   For "cell", one target per map with no cell (Target.at None): find_target() names the cell.
   @param game    the game
   @param attach  an attach point
   @return targets
   @throws ConnectorError for an unknown attach point
   """
   if attach not in ATTACH_POINTS:
      raise ConnectorError(f"there is no attach point {attach!r} (there are {', '.join(ATTACH_POINTS)})")
   if attach == "project":
      return [Target("project", "project", game.project_path, ())]
   out: list[Target] = []
   world_path = game.path_of("maps")
   if attach == "world" and world_path is not None:
      out.append(Target("world", "world", world_path, ()))
   if attach in ("map", "cell", "layer") and world_path is not None:
      world = game.read(world_path)
      for name, rel in ((world.get("maps") or {}) if isinstance(world, dict) else {}).items():
         map_path = (world_path.parent / rel).resolve()
         if attach != "layer":
            out.append(Target(attach, name, map_path, ()))
            continue
         data = game.read(map_path)
         for n, layer in enumerate(data.get("layers") or [] if isinstance(data, dict) else []):
            if isinstance(layer, dict):
               out.append(Target("layer", f"{name}/{layer.get('bg', n)}", map_path, ("layers", n)))
   if attach in BATTLE_LISTS and game.path_of("battle") is not None:
      path = game.path_of("battle")
      key = BATTLE_LISTS[attach]
      data = game.read(path)
      for n, entry in enumerate(data.get(key) or [] if isinstance(data, dict) else []):
         if isinstance(entry, dict) and isinstance(entry.get("name"), str):
            out.append(Target(attach, entry["name"], path, (key, n)))
   if attach == "asset" and game.path_of("assets") is not None:
      path = game.path_of("assets")
      data = game.read(path)
      for n, entry in enumerate(data.get("assets") or [] if isinstance(data, dict) else []):
         if isinstance(entry, dict) and isinstance(entry.get("key"), str):
            out.append(Target("asset", entry["key"], path, ("assets", n)))
   return out


def find_target(game: Game, attach: str, name: str) -> Target:
   """!The target of one record.
   @param game    the game
   @param attach  its attach point
   @param name    its name ("MAP@x,y" for a cell)
   @return the target
   @throws ConnectorError when there is no such record
   """
   if attach == "cell":
      map_name, at = parse_cell(name)
      for t in targets(game, "cell"):
         if t.name == map_name:
            return Target("cell", name, t.path, (), at)
      raise ConnectorError(f"there is no map {map_name!r}")
   for t in targets(game, attach):
      if t.name == name:
         return t
   raise ConnectorError(f"there is no {attach} {name!r} (a block goes on a record of the file itself)")


def _holder(data: Json, target: Target) -> dict[str, Json]:
   """!The object a target's block lives in.
   @param data    the file's data
   @param target  the target
   @return the object
   @throws ConnectorError when the file no longer has it
   """
   node = data
   for step in target.pointer:
      try:
         node = node[step]
      except (KeyError, IndexError, TypeError):
         node = None
         break
   if not isinstance(node, dict):
      raise ConnectorError(f"{target.path}: {target.attach} {target.name!r} has moved; read the file again")
   return node


def get_block(data: Json, target: Target, plugin_id: str) -> Json:
   """!A plugin's block on a record, as written.
   @param data       the file's data
   @param target     the record
   @param plugin_id  the plugin
   @return the block (without a cell's "at"), or None when it has none
   """
   holder = _holder(data, target)
   if target.at is None:
      ext = holder.get(EXT_KEY)
      return ext.get(plugin_id) if isinstance(ext, dict) else None
   cells = holder.get(CELL_EXT_KEY)
   for entry in (cells or {}).get(plugin_id) or [] if isinstance(cells, dict) else []:
      if isinstance(entry, dict) and entry.get(CELL_AT_KEY) == list(target.at):
         return {k: v for k, v in entry.items() if k != CELL_AT_KEY}
   return None


def set_block(data: Json, target: Target, plugin_id: str, block: Json) -> None:
   """!Sets or removes a plugin's block on a record, changing the data in place. An "ext" (or a cell list) left
   empty is removed, so a file without blocks stays as it was.
   @param data       the file's data
   @param target     the record
   @param plugin_id  the plugin
   @param block      the block (an object; a cell's without "at"), or None to remove it
   @return None
   """
   holder = _holder(data, target)
   if target.at is None:
      ext = holder.setdefault(EXT_KEY, {})
      if block is None:
         ext.pop(plugin_id, None)
      else:
         ext[plugin_id] = block
      if not ext:
         del holder[EXT_KEY]
      return
   cells = holder.setdefault(CELL_EXT_KEY, {})
   here = list(target.at)
   kept = [e for e in cells.get(plugin_id) or [] if not (isinstance(e, dict) and e.get(CELL_AT_KEY) == here)]
   if block is not None:
      kept.append({CELL_AT_KEY: list(target.at), **block})
      kept.sort(key=lambda e: tuple(e.get(CELL_AT_KEY, ())) if isinstance(e, dict) else ())
   if kept:
      cells[plugin_id] = kept
   else:
      cells.pop(plugin_id, None)
   if not cells:
      del holder[CELL_EXT_KEY]


def cells_of(data: Json, plugin_id: str) -> list[tuple[int, int]]:
   """!The cells of a map that carry a plugin's block.
   @param data       the map's data
   @param plugin_id  the plugin
   @return [(x, y)] as written
   """
   cells = data.get(CELL_EXT_KEY) if isinstance(data, dict) else None
   out = []
   for entry in (cells or {}).get(plugin_id) or [] if isinstance(cells, dict) else []:
      at = entry.get(CELL_AT_KEY) if isinstance(entry, dict) else None
      if isinstance(at, list) and len(at) == CELL_AT_LENGTH:
         out.append((at[0], at[1]))
   return out


# ---- loading plugins -----------------------------------------------------------------------------------------------

@dataclass
class Plugin:
   """!A loaded connector."""

   ## Its id.
   plugin_id: str
   ## Its folder.
   folder: Path
   ## What register(c) declared.
   spec: ConnectorSpec
   ## Its title.
   title: str = ""
   ## Its source files (the depfile).
   files: list[Path] = field(default_factory=list)


def check_purity(plugin_id: str, folder: Path, sources: Sequence[Path], wheels: Sequence[str]) -> None:
   """!The pure-Python rule (D-10 §2) for connector code.
   @param plugin_id  the plugin
   @param folder     its folder
   @param sources    the connector's Python files
   @param wheels     the wheels it vendors
   @return None
   @throws ConnectorError naming the plugin and the file
   """
   for f in sorted(folder.rglob("*")):
      if f.suffix.lower() in NATIVE_SUFFIXES:
         raise ConnectorError(f"plugin {plugin_id}: {f.relative_to(folder)} is native code; plugins are pure Python")
   for w in wheels:
      if not w.endswith(f"-{PURE_WHEEL}.whl"):
         raise ConnectorError(f"plugin {plugin_id}: wheel {w} is not {PURE_WHEEL}")
   for f in sources:
      try:
         tree = ast.parse(f.read_text(encoding="utf-8"), str(f))
      except (OSError, SyntaxError) as e:
         raise ConnectorError(f"plugin {plugin_id}: {f.relative_to(folder)}: {e}") from None
      for node in ast.walk(tree):
         names = []
         if isinstance(node, ast.Import):
            names = [a.name for a in node.names]
         elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            names = [node.module]
         for name in names:
            if name.split(".")[0] in REFUSED_IMPORTS:
               raise ConnectorError(f"plugin {plugin_id}: {f.relative_to(folder)}:{node.lineno} imports {name}; "
                                    "connectors are pure Python and run in headless builds")


def _import_connector(plugin_id: str, folder: Path, module: str) -> Any:
   """!Imports a connector module under a private name (two plugins may use the same package name; a plugin's
   modules import each other relatively).
   @param plugin_id  the plugin
   @param folder     its folder
   @param module     "package.module" relative to the folder
   @return the module
   @throws ConnectorError when it cannot be imported
   """
   top, _sep, rest = module.partition(".")
   alias = "capsule_connector_" + re.sub(r"\W", "_", plugin_id) + "_" + top
   package = folder / top
   try:
      if alias not in sys.modules:
         if (package / "__init__.py").is_file():
            spec = importlib.util.spec_from_file_location(alias, package / "__init__.py",
                                                          submodule_search_locations=[str(package)])
         elif (folder / f"{top}.py").is_file() and not rest:
            spec = importlib.util.spec_from_file_location(alias, folder / f"{top}.py")
         else:
            raise ConnectorError(f"plugin {plugin_id}: no module {module!r} in {folder}")
         loaded = importlib.util.module_from_spec(spec)
         sys.modules[alias] = loaded
         try:
            spec.loader.exec_module(loaded)
         except BaseException:
            del sys.modules[alias]
            raise
      return importlib.import_module(alias + (f".{rest}" if rest else ""))
   except ConnectorError:
      raise
   except Exception as e:   # a plugin's own error while importing: named, not a crash of the build
      raise ConnectorError(f"plugin {plugin_id}: importing {module} failed: {type(e).__name__}: {e}") from None


def load_plugin(folder: str | Path) -> Plugin | None:
   """!Loads one plugin's connector.
   @param folder  the plugin folder
   @return the plugin, or None when it has no connector (an extension only)
   @throws ConnectorError when plugin.json, the purity rule or register() fails
   """
   folder = Path(folder).resolve()
   manifest_path = folder / "plugin.json"
   try:
      info = json.loads(manifest_path.read_text(encoding="utf-8"))
   except OSError as e:
      raise ConnectorError(f"{manifest_path}: cannot read ({e.strerror})") from None
   except json.JSONDecodeError as e:
      raise ConnectorError(f"{manifest_path}: {e}") from None
   if not isinstance(info, dict) or info.get("schema") != PLUGIN_SCHEMA:
      raise ConnectorError(f"{manifest_path}: schema must be {PLUGIN_SCHEMA}")
   pid = info.get("id")
   if not isinstance(pid, str) or not PLUGIN_ID.fullmatch(pid):
      raise ConnectorError(f"{manifest_path}: id must be reverse-domain style (e.g. \"acme.party_moods\")")
   part = info.get("connector")
   if part is None:
      return None
   module = part.get("module") if isinstance(part, dict) else None
   if not isinstance(module, str) or not module:
      raise ConnectorError(f"{manifest_path}: connector is {{\"module\": \"package.module\"}}")
   if info.get("api") not in CONNECTOR_APIS:
      raise ConnectorError(f"plugin {pid}: has a connector for plugin API {info.get('api')!r}; this engine runs "
                           f"connectors for API {', '.join(str(a) for a in CONNECTOR_APIS)}")
   top = module.split(".")[0]
   sources = sorted((folder / top).rglob("*.py")) if (folder / top).is_dir() else [folder / f"{top}.py"]
   check_purity(pid, folder, sources, info.get("pure_python_deps", []))
   connector = _import_connector(pid, folder, module)
   register = getattr(connector, "register", None)
   if not callable(register):
      raise ConnectorError(f"plugin {pid}: {module} defines no register(c)")
   spec = ConnectorSpec(pid)
   try:
      register(spec)
   except ConnectorError:
      raise
   except Exception as e:
      raise ConnectorError(f"plugin {pid}: register() failed: {type(e).__name__}: {e}") from None
   if not spec.attach_points:
      raise ConnectorError(f"plugin {pid}: its connector attaches nowhere (c.attach(point, c.block(...)))")
   return Plugin(pid, folder, spec, str(info.get("title") or pid), [manifest_path] + list(sources))


def load_plugins(game: Game) -> list[Plugin]:
   """!The connectors of the plugins a game lists, by id.
   @param game  the game
   @return plugins (extension-only plugins left out)
   @throws ConnectorError when one fails to load, or two share an id
   """
   out: dict[str, Plugin] = {}
   for folder in game.plugin_folders():
      plugin = load_plugin(folder)
      if plugin is None:
         continue
      if plugin.plugin_id in out:
         raise ConnectorError(f"two plugins are {plugin.plugin_id}: {out[plugin.plugin_id].folder} and {folder}")
      out[plugin.plugin_id] = plugin
   return [out[k] for k in sorted(out)]


# ---- the build -----------------------------------------------------------------------------------------------------

class Context:
   """!What a plugin's hooks receive: its records, references, problems and (emit) its outputs."""

   def __init__(self, game: Game, plugin: Plugin, records: list[Record], problems: list[Problem]) -> None:
      """!Creates the context of one plugin.
      @param game      the game
      @param plugin    the plugin
      @param records   its records
      @param problems  the build's problems (appended to)
      """
      ## The plugin's id.
      self.plugin_id = plugin.plugin_id
      ## The game (read-only use).
      self._game = game
      ## Its records, in manifest order.
      self._records = records
      ## The build's problems.
      self._problems = problems
      ## Outputs written: relative name -> text or bytes.
      self.outputs = {}
      ## Whether write() is allowed (emit hooks only).
      self._emitting = False

   def records(self, attach: str | None = None) -> list[Record]:
      """!The records carrying this plugin's blocks.
      @param attach  only those at this attach point, or None for all
      @return records, in manifest order
      """
      return [r for r in self._records if attach is None or r.attach == attach]

   def names(self, kind: str) -> dict[str, int]:
      """!Every record of a kind (whether it carries a block or not), with its number.
      @param kind  one of REF_KINDS except asset
      @return name -> number
      """
      return dict(self._game.numbers(kind))

   def resolve(self, kind: str, name: str, where: str = "") -> int:
      """!A record's number by name.
      @param kind   one of REF_KINDS
      @param name   its name
      @param where  location for messages
      @return the number
      @throws ConnectorError when there is none
      """
      return self._game.resolve(kind, name, where or f"[{self.plugin_id}]")

   def error(self, record: Record | None, message: str) -> None:
      """!Reports an error (the build fails after the hooks ran).
      @param record   the record it is about, or None
      @param message  what is wrong
      @return None
      """
      self._report("error", record, message)

   def warning(self, record: Record | None, message: str) -> None:
      """!Reports a warning (printed; the build goes on).
      @param record   the record it is about, or None
      @param message  what is worth a look
      @return None
      """
      self._report("warning", record, message)

   def _report(self, severity: str, record: Record | None, message: str) -> None:
      """!Adds a problem.
      @param severity  "error" or "warning"
      @param record    its record, or None
      @param message   the text
      @return None
      """
      where = f"{record.where}: " if record is not None else ""
      key = (record.attach, record.name) if record is not None else ("", "")
      self._problems.append(Problem(severity, self.plugin_id, f"{where}{message}", key))

   def write(self, name: str, content: str | bytes) -> None:
      """!Writes an output (emit hooks only): OUT_DIR/PLUGIN_ID/NAME. A .cpp output is compiled into the game.
      @param name     a relative name with one of OUTPUT_SUFFIXES (folders allowed)
      @param content  text or bytes
      @return None
      @throws ConnectorError outside emit, for a bad name, or one written twice
      """
      if not self._emitting:
         raise ConnectorError(f"plugin {self.plugin_id}: write() is for emit hooks")
      if not OUTPUT_NAME.fullmatch(name) or ".." in name.split("/") or Path(name).suffix not in OUTPUT_SUFFIXES:
         raise ConnectorError(f"plugin {self.plugin_id}: cannot write {name!r} (a relative name ending in "
                              f"{', '.join(OUTPUT_SUFFIXES)})")
      if name in self.outputs:
         raise ConnectorError(f"plugin {self.plugin_id}: writes {name} twice")
      if not isinstance(content, (str, bytes)):
         raise ConnectorError(f"plugin {self.plugin_id}: {name}: content is text or bytes")
      self.outputs[name] = content


@dataclass
class Result:
   """!What a build found and made."""

   ## The plugins loaded.
   plugins: list[Plugin]
   ## Records by plugin id.
   records: dict[str, list[Record]]
   ## Errors and warnings.
   problems: list[Problem]
   ## Outputs by plugin id: name -> content.
   outputs: dict[str, dict[str, str | bytes]]
   ## Files read (the depfile).
   deps: list[Path]

   def errors(self) -> list[Problem]:
      """!Only the errors.
      @return problems
      """
      return [p for p in self.problems if p.severity == "error"]


def _cell_records(plugin_id: str, raw: Json, name: str, where: str, chunks: set[tuple[int, int]] | None,
                  problems: list[Problem]) -> list[tuple[str, str, Json]]:
   """!A map's cell blocks of one plugin: [{"at": [x, y], ...}, ...].
   @param plugin_id  the plugin
   @param raw        its list
   @param name       the map
   @param where      the location of the list
   @param chunks     the map's chunk positions (a cell must lie in one), or None to skip that check
   @param problems   problems (appended to)
   @return [(record name, where, block without "at")]
   """
   if not isinstance(raw, list):
      problems.append(Problem("error", plugin_id, f"{where}.{plugin_id}: a list of {{\"at\": [x, y], ...}} is needed"))
      return []
   out = []
   seen = set()
   for n, entry in enumerate(raw):
      at = entry.get(CELL_AT_KEY) if isinstance(entry, dict) else None
      here = f"{where}.{plugin_id}[{n}]"
      if not (isinstance(at, list) and len(at) == CELL_AT_LENGTH
              and all(isinstance(v, int) and not isinstance(v, bool) for v in at)):
         problems.append(Problem("error", plugin_id, f"{here}: \"at\" is [x, y], two whole numbers"))
         continue
      x, y = at
      if (x, y) in seen:
         problems.append(Problem("error", plugin_id, f"{here}: cell {x},{y} is given twice"))
         continue
      seen.add((x, y))
      # Maps reach left of and above 0 too; floor division puts a negative cell in its chunk (-1 // 8 == -1).
      if chunks is not None and (x // CHUNK_CELLS, y // CHUNK_CELLS) not in chunks:
         problems.append(Problem("error", plugin_id, f"{here}: cell {x},{y} is outside the map's chunks"))
         continue
      out.append((f"{name}@{x},{y}", here, {k: v for k, v in entry.items() if k != CELL_AT_KEY}))
   return out


def collect(game: Game, plugins: Sequence[Plugin], problems: list[Problem]) -> dict[str, list[Record]]:
   """!Finds every block, refusing those of plugins not loaded or not attaching there, and checks each against its
   schema.
   @param game      the game
   @param plugins   the loaded connectors
   @param problems  problems (appended to)
   @return records by plugin id (only blocks that passed their schema)
   """
   by_id = {p.plugin_id: p for p in plugins}
   records: dict[str, list[Record]] = {p.plugin_id: [] for p in plugins}
   for attach, name, where, number, ext in game.blocks():
      if ext is None:
         continue
      key = CELL_EXT_KEY if attach == "cell" else EXT_KEY
      here = where if attach == "cell" else f"{where}: {key}"
      if not isinstance(ext, dict):
         problems.append(Problem("error", "build", f"{here}: an object of plugin id -> block is needed"))
         continue
      for plugin_id, block in ext.items():
         plugin = by_id.get(plugin_id)
         if plugin is None:
            problems.append(Problem("error", plugin_id, f"{here}.{plugin_id}: the game loads no plugin "
                                                        f"{plugin_id!r} with a connector (project.json \"plugins\")"))
            continue
         schema = plugin.spec.attach_points.get(attach)
         if schema is None:
            problems.append(Problem("error", plugin_id, f"{here}.{plugin_id}: {plugin_id} does not attach at "
                                                        f"{attach} (it attaches at "
                                                        f"{', '.join(plugin.spec.attach_points)})"))
            continue
         entries = [(name, f"{here}.{plugin_id}", block)]
         if attach == "cell":
            entries = _cell_records(plugin_id, block, name, here, game.map_chunks(name), problems)
         for record_name, record_where, value in entries:
            messages = schema.check(value, record_where)
            for m in messages:
               problems.append(Problem("error", plugin_id, m, (attach, record_name)))
            if not messages:
               records[plugin_id].append(Record(attach, record_name, record_where, value, number))
   return records


def run(project_path: str | Path, sources: Mapping[Path, Json] | None = None, emit: bool = True) -> Result:
   """!Loads the plugins, collects and checks the blocks, resolves them and runs the hooks (no files written).
   @param project_path  reference/project.json
   @param sources       {Path: data} replacing files, or None
   @param emit          whether the emit hooks run
   @return the result (problems instead of exceptions for everything about blocks and hooks)
   @throws ConnectorError when the project or a plugin cannot be loaded
   """
   game = Game(project_path, sources)
   plugins = load_plugins(game)
   problems: list[Problem] = []
   records = collect(game, plugins, problems)
   outputs: dict[str, dict[str, str | bytes]] = {}
   if any(p.severity == "error" for p in problems):
      return Result(plugins, records, problems, outputs, _deps(game, plugins))
   for plugin in plugins:
      mine = records[plugin.plugin_id]
      ctx = Context(game, plugin, mine, problems)
      before = len([p for p in problems if p.severity == "error"])
      for record in mine:
         try:
            record.values = plugin.spec.attach_points[record.attach].resolve(record.block, ctx, record.where)
         except ConnectorError as e:
            problems.append(Problem("error", plugin.plugin_id, str(e), (record.attach, record.name)))
      if len([p for p in problems if p.severity == "error"]) > before:
         continue
      stages = [("resolve", h) for h in plugin.spec.resolvers] + [("validate", h) for h in plugin.spec.validators]
      for stage, hook in stages:
         for record in mine:
            _call(plugin, stage, problems, hook, record, ctx)
      if emit and len([p for p in problems if p.severity == "error"]) == before:
         ctx._emitting = True
         for hook in plugin.spec.emitters:
            _call(plugin, "emit", problems, hook, ctx)
         ctx._emitting = False
         outputs[plugin.plugin_id] = ctx.outputs
   return Result(plugins, records, problems, outputs, _deps(game, plugins))


def _call(plugin: Plugin, stage: str, problems: list[Problem], hook: Callable[..., None], *args: Any) -> None:
   """!Runs a hook; its own errors become problems naming the plugin and the stage.
   @param plugin    the plugin
   @param stage     "resolve", "validate" or "emit"
   @param problems  problems (appended to)
   @param hook      the hook
   @param args      its arguments
   @return None
   """
   try:
      hook(*args)
   except ConnectorError as e:
      problems.append(Problem("error", plugin.plugin_id, str(e)))
   except Exception as e:   # a plugin's bug: named, the build fails cleanly
      problems.append(Problem("error", plugin.plugin_id, f"its {stage} hook failed: {type(e).__name__}: {e}"))


def _deps(game: Game, plugins: Sequence[Plugin]) -> list[Path]:
   """!Every file the build read.
   @param game     the game
   @param plugins  the plugins
   @return paths, each once
   """
   out = list(game.deps)
   for plugin in plugins:
      for f in plugin.files:
         if f not in out:
            out.append(f)
   return out


# ---- writing -------------------------------------------------------------------------------------------------------

def _write_if_changed(path: Path, content: str | bytes) -> None:
   """!Writes a file only when its content differs (the build does not recompile what did not change).
   @param path     the file
   @param content  text (UTF-8, LF) or bytes
   @return None
   """
   data = content.encode("utf-8") if isinstance(content, str) else content
   if path.is_file() and path.read_bytes() == data:
      return
   path.parent.mkdir(parents=True, exist_ok=True)
   path.write_bytes(data)


def aggregate(outputs: Mapping[str, Mapping[str, str | bytes]]) -> str:
   """!connectors.cpp: includes every compiled output, by plugin id and name.
   @param outputs  plugin id -> name -> content
   @return the source
   """
   lines = ["// Generated by tools/connectors.py: the C++ sources the game's connectors wrote. Do not edit.", ""]
   for plugin_id in sorted(outputs):
      for name in sorted(outputs[plugin_id]):
         if OUTPUT_SUFFIXES.get(Path(name).suffix):
            lines.append(f'#include "{plugin_id}/{name}"')
   return "\n".join(lines) + "\n"


def write(result: Result, out_dir: str | Path) -> list[Path]:
   """!Writes a build's outputs and connectors.cpp, and removes files a plugin no longer writes.
   @param result   a run() without errors
   @param out_dir  generated/GAME/ext
   @return the files written or kept
   """
   out_dir = Path(out_dir)
   kept: list[Path] = []
   for plugin_id, files in result.outputs.items():
      for name, content in files.items():
         path = out_dir / plugin_id / name
         _write_if_changed(path, content)
         kept.append(path.resolve())
   aggregate_path = out_dir / AGGREGATE_NAME
   _write_if_changed(aggregate_path, aggregate(result.outputs))
   # The build's output: newer than its inputs after every run, even unchanged (Make has no restat, so an old time
   # stamp would rerun the step on every build). Only this file is touched; the plugins' files keep theirs.
   aggregate_path.touch()
   kept.append(aggregate_path.resolve())
   if out_dir.is_dir():
      for f in sorted(out_dir.rglob("*"), reverse=True):
         if f.is_file() and f.resolve() not in kept:
            f.unlink()
         elif f.is_dir() and not any(f.iterdir()):
            f.rmdir()
   return kept


def write_depfile(path: str | Path, target: Path, deps: Sequence[Path]) -> None:
   """!A make-style dependency file.
   @param path    the depfile
   @param target  the output it is for
   @param deps    the files read
   @return None
   """
   def esc(p: Path) -> str:
      """!A path escaped for make.
      @param p  the path
      @return text
      """
      return str(p).replace("\\", "/").replace(" ", "\\ ")

   _write_if_changed(Path(path), f"{esc(target)}: " + " \\\n  ".join(esc(d) for d in deps) + "\n")


def describe_attach_points() -> str:
   """!The attach points, one a line.
   @return text
   """
   return "\n".join(f"{p.name}: {p.where}" for p in ATTACH_POINTS.values())


@contextlib.contextmanager
def _clean_imports() -> Iterator[None]:
   """!Drops the plugins' private modules afterwards (a build or check imports them afresh).
   @return the block
   """
   before = set(sys.modules)
   try:
      yield
   finally:
      for name in set(sys.modules) - before:
         if name.startswith("capsule_connector_"):
            del sys.modules[name]


def main(argv: Sequence[str]) -> int:
   """!Command line (see the module's notes).
   @param argv  arguments without the program name
   @return exit status
   """
   import argparse
   p = argparse.ArgumentParser(prog="connectors.py", description="Project data of a game's connector plugins.")
   sub = p.add_subparsers(dest="command", required=True)
   b = sub.add_parser("build", help="check, compile and write the connectors' outputs")
   b.add_argument("project")
   b.add_argument("--out-dir", required=True)
   b.add_argument("--depfile")
   c = sub.add_parser("check", help="the checks only")
   c.add_argument("project")
   sub.add_parser("attach-points", help="where blocks may attach")
   args = p.parse_args(argv)
   if args.command == "attach-points":
      print(describe_attach_points())
      return 0
   with _clean_imports():
      try:
         result = run(args.project, emit=args.command == "build")
      except ConnectorError as e:
         print(f"{args.project}: error: {e}", file=sys.stderr)
         return 1
   for problem in result.problems:
      print(problem.text(), file=sys.stderr)
   if result.errors():
      return 1
   if args.command == "build":
      write(result, args.out_dir)
      if args.depfile:
         write_depfile(args.depfile, Path(args.out_dir) / AGGREGATE_NAME, result.deps)
   return 0


if __name__ == "__main__":
   sys.exit(main(sys.argv[1:]))
