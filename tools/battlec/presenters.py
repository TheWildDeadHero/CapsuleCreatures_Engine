"""!Presenters of the battle compiler: who shows a party on screen and speaks
for it (capsule::battle::PresenterData), their entrances and exits, and the
presenter sequences they may play.
"""
from __future__ import annotations

import json
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

import sequence_compiler as seqc
import text_encoder
from sequence_compiler import asset_key

from .common import I8_MAX, I8_MIN, PAIR, U8_MAX, Json, ManifestError, check_keys, encode, integer, register_keys

## Presenters per party (capsule::battle::kMaxPresenters).
MAX_PRESENTERS = 2
## Presenter motions, in capsule::battle::Motion order.
MOTIONS = ["none", "slide", "fade", "sequence", "stay"]
## Slide edges, in capsule::battle::Edge order.
EDGES = ["side", "left", "right", "top", "bottom"]
## When a presenter leaves: at its party's first send-out, or right after the intro.
EXIT_WHEN = ["send_out", "after_intro"]
## Default frames of a moving entrance or exit.
DEFAULT_MOTION_FRAMES = 24
## Presenter flag bits (capsule::battle::PresenterFlag): leaves after the intro.
PRESENTER_EXIT_AFTER_INTRO = 1
## Presenter flag: never leaves.
PRESENTER_STAYS = 2
## Presenter flag: comes back when its party is defeated or wins.
PRESENTER_RETURNS = 4
## Sprite slots a presenter sequence may use (presenter_view: battlers 0-5, presenters 6-13, sequences 14-15).
PRESENTER_SEQUENCE_SLOTS = (14, 15)
## Sequence ops a presenter sequence may use (sprites, waits, blending, sound, labels and loops).
PRESENTER_SEQUENCE_OPS = {"end", "wait", "wait_for", "blend", "blend_ramp", "spr_load", "spr_show", "spr_pos",
                          "spr_move", "spr_anim", "spr_obj_window", "spr_affine", "spr_blend", "song", "song_fade",
                          "sfx", "jump", "repeat", "next", "goto"}
## Sequence ops whose first argument is a sprite slot.
SPRITE_SLOT_OPS = {"spr_load", "spr_show", "spr_pos", "spr_move", "spr_anim", "spr_obj_window", "spr_affine",
                   "spr_blend"}
## Sequence stage flag a presenter sequence may not set (it would clear the battle screen).
STAGE_RESET_VISUALS = 1
## Index of the flags in a compiled sequence stage record.
STAGE_FLAGS_FIELD = 2
## Placeholders of presenter texts.
PRESENTER_TEXT_ARGS = {"party"}
## A presenter's texts (manifest keys), in capsule::battle::PresenterData::texts order (kTextIntro, kTextDefeat,
## kTextVictory).
PRESENTER_TEXTS = ("intro", "defeat", "victory")
## The motions that do not move (no frames, edge or sequence; compile_motion()).
STILL_MOTIONS = ("none", "stay")


def compile_motion(v: Json, kind: str, where: str, base: Path, deps: list[Path] | None,
                   codepage: text_encoder.CodePage, symbol: str) -> dict[str, Any]:
   """!A presenter's entrance or exit.
   @param v         {style, frames, from, when (exit), sequence}, or None (style none)
   @param kind      "enter" or "exit"
   @param where     location
   @param base      folder sequence paths are relative to
   @param deps      list collecting files read (depfile), or None
   @param codepage  the game's code page
   @param symbol    C++ symbol of its sequence
   @return {style, frames, from, when, seq} (seq: the sequence .inc text, or None)
   """
   v = {} if v is None else v
   check_keys(v, kind, where)
   style = v.get("style", MOTIONS[0])
   styles = MOTIONS if kind == "exit" else [m for m in MOTIONS if m != "stay"]
   if style not in styles:
      raise ManifestError(f"{where}.style: one of {', '.join(styles)}")
   still = style in STILL_MOTIONS
   frames = integer(v.get("frames", 0 if still else DEFAULT_MOTION_FRAMES), 0 if still else 1, 0 if still else U8_MAX,
                    f"{where}.frames")
   edge = v.get("from", EDGES[0])
   if edge not in EDGES:
      raise ManifestError(f"{where}.from: one of {', '.join(EDGES)}")
   when = v.get("when", EXIT_WHEN[0])
   if when not in EXIT_WHEN:
      raise ManifestError(f"{where}.when: one of {', '.join(EXIT_WHEN)}")
   if (style == "sequence") != ("sequence" in v):
      raise ManifestError(f"{where}: the sequence style needs a sequence (and only it takes one)")
   seq = None
   if style == "sequence":
      seq = compile_presenter_sequence(v["sequence"], where, base, deps, codepage, symbol)
   return {"style": MOTIONS.index(style), "frames": frames, "from": EDGES.index(edge),
           "when": EXIT_WHEN.index(when), "seq": seq, "symbol": symbol if seq is not None else None}


def compile_presenter_sequence(rel: Json, where: str, base: Path, deps: list[Path] | None,
                               codepage: text_encoder.CodePage, symbol: str) -> str:
   """!Compiles a presenter's sequence (as location_compiler does) and checks
   that it only moves its own sprites: slots PRESENTER_SEQUENCE_SLOTS, the ops
   in PRESENTER_SEQUENCE_OPS, no reset_visuals stage.
   @param rel       path relative to the battle manifest
   @param where     location
   @param base      the manifest's folder
   @param deps      list collecting files read, or None
   @param codepage  the game's code page
   @param symbol    C++ symbol
   @return the .inc text (without its include guard)
   """
   if not isinstance(rel, str) or not rel:
      raise ManifestError(f"{where}.sequence: a path")
   p = (base / rel).resolve()
   if deps is not None:
      deps.append(p)
   try:
      comp = seqc.Compiler(json.loads(p.read_text(encoding="utf-8")), codepage=codepage).run()
   except OSError as e:
      raise ManifestError(f"{where}: cannot read {p}: {e.strerror}") from None
   except json.JSONDecodeError as e:
      raise ManifestError(f"{where}: {p.name}: invalid JSON ({e})") from None
   except (seqc.ManifestError, KeyError, TypeError, ValueError, AttributeError) as e:
      raise ManifestError(f"{where}: {p.name} does not compile: {e}") from None
   for op, a, _b, _c in comp.cmds:
      name = seqc.OPS[op]
      if name not in PRESENTER_SEQUENCE_OPS:
         raise ManifestError(f"{where}: {p.name}: {name} is not allowed in a presenter sequence (allowed: "
                             f"{', '.join(sorted(PRESENTER_SEQUENCE_OPS))})")
      if name in SPRITE_SLOT_OPS and a not in PRESENTER_SEQUENCE_SLOTS:
         raise ManifestError(f"{where}: {p.name}: sprite slot {a}: presenter sequences use slots "
                             f"{' and '.join(str(x) for x in PRESENTER_SEQUENCE_SLOTS)}")
   for rec in comp.stage_recs:
      if rec[STAGE_FLAGS_FIELD] & STAGE_RESET_VISUALS:
         raise ManifestError(f"{where}: {p.name}: a presenter sequence may not reset the visuals")
   text = comp.emit_inc(symbol)

   # Embedded in battle.cpp: the include guard of a stand-alone .inc does not belong.
   return "\n".join(ln for ln in text.splitlines() if ln.strip() != "#pragma once")


def compile_presenter(pd: Json, index: int, where: str, base: Path, deps: list[Path] | None,
                      codepage: text_encoder.CodePage, terms: Mapping[str, Any],
                      field: Mapping[str, int] | None = None) -> dict[str, Any]:
   """!One presenter.
   @param pd        manifest object
   @param index     its index (names its sequences' symbols)
   @param where     location
   @param base      the manifest's folder (sequence paths)
   @param deps      list collecting files read, or None
   @param codepage  the game's code page
   @param terms     compile_terms() result
   @param field     the game's field placeholders, or None
   @return compiled presenter
   """
   check_keys(pd, "presenter", where)
   sprite = pd.get("sprite")
   if sprite is not None and (not isinstance(sprite, str) or not sprite.startswith("sprite:")):
      raise ManifestError(f"{where}.sprite: a sprite asset key (\"sprite:NAME\")")
   at = pd.get("at", [0, 0])
   if not isinstance(at, list) or len(at) != PAIR:
      raise ManifestError(f"{where}.at: [dx, dy]")
   dx, dy = (integer(v, I8_MIN, I8_MAX, f"{where}.at") for v in at)
   enter = compile_motion(pd.get("enter"), "enter", f"{where}.enter", base, deps, codepage,
                          f"kBattlePresenter{index}Enter")
   exit_ = compile_motion(pd.get("exit"), "exit", f"{where}.exit", base, deps, codepage,
                          f"kBattlePresenter{index}Exit")
   returns = pd.get("returns", False)
   if not isinstance(returns, bool):
      raise ManifestError(f"{where}.returns: true or false")
   flags = (PRESENTER_EXIT_AFTER_INTRO if exit_["when"] == EXIT_WHEN.index("after_intro") else 0) | \
           (PRESENTER_STAYS if exit_["style"] == MOTIONS.index("stay") else 0) | \
           (PRESENTER_RETURNS if returns else 0)
   texts = {k: (encode(pd[k], f"{where}.{k}", PRESENTER_TEXT_ARGS, codepage, terms, field=field) if k in pd
                else None)
            for k in PRESENTER_TEXTS}
   return {"name": pd["name"], "sprite": asset_key(sprite, f"{where}.sprite") if sprite else 0, "dx": dx, "dy": dy,
           "enter": enter, "exit": exit_, "flags": flags, "texts": texts}


register_keys({
   "presenter": {"name", "sprite", "at", "enter", "exit", "returns", *PRESENTER_TEXTS},
   "enter": {"style", "frames", "from", "sequence"},
   "exit": {"style", "frames", "from", "when", "sequence"},
})


def compile_presenters(entries: Sequence[Json], path: str | Path, where: str, deps: list[Path] | None,
                       codepage: text_encoder.CodePage, terms: Mapping[str, Any],
                       field: Mapping[str, int] | None = None) -> list[dict[str, Any]]:
   """!Presenters (who shows a party on screen).
   @param entries   the manifest's presenters
   @param path      battle.json (sequence paths are relative to its folder)
   @param where     location
   @param deps      list collecting files read, or None
   @param codepage  the game's code page
   @param terms     compile_terms() result
   @param field     the game's field placeholders (their texts may use them), or None
   @return compiled presenters
   """
   base_dir = Path(path).resolve().parent
   return [compile_presenter(pd, i, f"{where}: presenters[{i}] ({pd.get('name')})", base_dir, deps, codepage, terms,
                             field)
           for i, pd in enumerate(entries)]
