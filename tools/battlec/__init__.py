"""!The battle compiler's sections (tools/battle_compiler.py is the driver and
the command line; it re-exports every public name of these modules):

  - common: errors, value checks, text encoding, name lookups, manifest keys
  - layout: the creature record layout (roles, offsets, hash)
  - stats: stats, stages, move categories, traits, stat names
  - chart: types and the type chart
  - conditions: statuses, weathers, volatiles
  - rules: non-formula rules and the ruleset range check
  - progression: levels, growth tables, ranks, rewards
  - turn: formats and the battle's geometry caps
  - presenters: presenters and their sequences
  - content: moves, passives and items, species, parties
  - pipelines: the ruleset's pipelines bound to the battle, point handlers, the Python point runner
  - logic: the `.ceffect` sources and the effect bank of every move, passive, item, status and weather
  - transfer: the record transfer table
  - texts: engine messages and terms
  - emit: the generated C++ source and ids header

Each section module adds the manifest objects it reads to common.KEYS.
"""
from __future__ import annotations

import sys
from pathlib import Path

## The engine's tools/ folder: the sections import its other tools (ruleset_compiler, text_encoder, ...) by bare
## name.
_TOOLS = str(Path(__file__).resolve().parents[1])
if _TOOLS not in sys.path:
   sys.path.insert(0, _TOOLS)
