#!/usr/bin/env python3
"""!Move routes (D-17, RPG Maker XP's Set Move Route): the step words shared by
map object pages (tools/object_compiler.py, "route") and scripts
(tools/script_compiler.py, `route <obj> { ... }`), compiled to the byte code
of capsule/script/route.h. Not run on its own.

Steps (one per string in JSON, one per `;` in scripts):

    up | down | left | right [n]     n cells (1-255, default 1)
    random | toward | away           one step: at random, towards / away from the player
    forward | backward               one step the way it faces / backwards (facing kept)
    jump dx dy                       to a cell up to 8 away (jump 0 0: in place)
    wait n                           n frames (1-65535)
    face up | down | left | right | toward | away
    turn right | left | around | random
    set FLAG | clear FLAG            a flag of the scripts manifest
    speed n                          1-4 (2 = walking)
    frequency n                      1-5 (3 = default)
    walk_anim | step_anim | direction_fix | through | on_top  on | off
    sprite sprite:NAME
    show | hide
    in_place up | down | left | right [speed]   I4: one step's time walking in place facing there (speed 1-4)
    face original                    I4: back to the facing of its page (map objects)
    step NAME [arg]                  I4: a game route step (`step NAME;` declarations of the scripts; arg 0-255)

Route flags: repeat (start over at the end; such a route never finishes)
and skip (skip a blocked step instead of waiting until it is free).
"""
from __future__ import annotations

import struct
from collections.abc import Callable, Sequence

## Step opcodes in capsule::route::RouteOp order.
OPS = ["End", "Move", "Random", "Toward", "Away", "Forward", "Backward", "Jump", "Wait", "Face", "TurnRight",
       "TurnLeft", "TurnAround", "TurnRandom", "FaceToward", "FaceAway", "Flag", "Speed", "Frequency", "Option",
       "Sprite", "Visible",
       # I4 (D-29): a walk in place, back to the page's facing, a game route step
       "InPlace", "FaceOriginal", "Game"]
## Opcode name -> number.
OP = {n: i for i, n in enumerate(OPS)}
## Directions in field::Direction order.
DIRECTIONS = ["down", "up", "left", "right"]
## Route flag bits.
REPEAT, SKIP = 1, 2
## Option words -> bit (capsule::route::kOption...).
OPTIONS = {"walk_anim": 1, "step_anim": 2, "direction_fix": 4, "through": 8, "on_top": 16}
## Options when none are given (walking animation).
DEFAULT_OPTIONS = 1
## Longest route in bytes.
MAX_BYTES = 4096

## Words in a step with one argument (the step word and its argument).
WORDS_WITH_ARGUMENT = 2
## Index of a step's second argument in its words (`in_place up 3`).
SECOND_ARGUMENT = 2
## Words in a step with two arguments (jump dx dy).
WORDS_WITH_TWO_ARGUMENTS = 3
## Most cells one move step walks (the count is one byte).
MAX_MOVE_COUNT = 255
## Farthest a jump goes on each axis, in cells.
MAX_JUMP_DISTANCE = 8
## Mask keeping the low byte: a negative jump offset is stored as a signed byte (two's complement).
BYTE_MASK = 0xFF
## Longest wait in frames (the frame count is a u16).
MAX_WAIT_FRAMES = 65535
## Fastest movement speed (2 = walking).
MAX_SPEED = 4
## Walking speed (an in_place step's default).
DEFAULT_SPEED = 2
## Highest movement frequency (3 = default).
MAX_FREQUENCY = 5

## Callable(name) -> flag ID; raises for an unknown flag.
FlagIdFn = Callable[[str], int]
## Callable(name) -> a game route step's ID; raises for an unknown step (I4).
StepIdFn = Callable[[str], int]
## Highest argument of a game route step.
MAX_STEP_ARG = 255
## Callable("sprite:NAME") -> asset key.
SpriteKeyFn = Callable[[str], int]


class RouteError(Exception):
   """!A route step is malformed; the message says how."""
   pass


def _int(word: str, lo: int, hi: int, what: str) -> int:
   """!An integer word.
   @param word  the text
   @param lo    minimum
   @param hi    maximum
   @param what  name for messages
   @return the value
   """
   try:
      v = int(word, 0)
   except (TypeError, ValueError):
      raise RouteError(f"{what} must be a number, got {word!r}") from None
   if not lo <= v <= hi:
      raise RouteError(f"{what} must be {lo}-{hi}, got {v}")
   return v


def _onoff(words: Sequence[str], what: str) -> int:
   """!The on / off word of an option step.
   @param words  the step's words
   @param what   the option
   @return 1 or 0
   """
   if len(words) != WORDS_WITH_ARGUMENT or words[1] not in ("on", "off"):
      raise RouteError(f"{what} takes on or off")
   return 1 if words[1] == "on" else 0


def step(words: Sequence[str], flag_id: FlagIdFn, sprite_key: SpriteKeyFn, step_id: StepIdFn | None = None) -> bytes:
   """!Compiles one step.
   @param words       the step's words (e.g. ["up", "2"])
   @param flag_id     callable(name) -> flag ID; raises for an unknown flag
   @param sprite_key  callable("sprite:NAME") -> asset key
   @param step_id     callable(name) -> a game route step's ID (I4), or None (no game steps)
   @return bytes
   """
   if not words:
      raise RouteError("empty step")
   w, n = words[0], len(words)
   if w in DIRECTIONS:
      if n > WORDS_WITH_ARGUMENT:
         raise RouteError(f"{w} takes at most a count")
      count = _int(words[1], 1, MAX_MOVE_COUNT, f"{w} count") if n == WORDS_WITH_ARGUMENT else 1
      return bytes([OP["Move"], DIRECTIONS.index(w), count])
   simple = {"random": "Random", "toward": "Toward", "away": "Away", "forward": "Forward", "backward": "Backward",
             "show": "Visible", "hide": "Visible"}
   if w in simple:
      if n != 1:
         raise RouteError(f"{w} takes nothing")
      if w in ("show", "hide"):
         return bytes([OP["Visible"], 1 if w == "show" else 0])
      return bytes([OP[simple[w]]])
   if w == "jump":
      if n != WORDS_WITH_TWO_ARGUMENTS:
         raise RouteError("jump takes dx and dy")
      _, dx_word, dy_word = words
      dx = _int(dx_word, -MAX_JUMP_DISTANCE, MAX_JUMP_DISTANCE, "jump dx")
      dy = _int(dy_word, -MAX_JUMP_DISTANCE, MAX_JUMP_DISTANCE, "jump dy")

      # the offsets are signed bytes: masking stores a negative one in two's complement
      return bytes([OP["Jump"], dx & BYTE_MASK, dy & BYTE_MASK])
   if w == "wait":
      if n != WORDS_WITH_ARGUMENT:
         raise RouteError("wait takes frames")
      return bytes([OP["Wait"]]) + struct.pack("<H", _int(words[1], 1, MAX_WAIT_FRAMES, "wait frames"))
   if w == "face":
      faces = DIRECTIONS + ["toward", "away", "original"]
      if n != WORDS_WITH_ARGUMENT or words[1] not in faces:
         raise RouteError(f"face takes one of {', '.join(faces)}")
      if words[1] == "toward":
         return bytes([OP["FaceToward"]])
      if words[1] == "away":
         return bytes([OP["FaceAway"]])
      if words[1] == "original":
         return bytes([OP["FaceOriginal"]])
      return bytes([OP["Face"], DIRECTIONS.index(words[1])])
   if w == "turn":
      turns = {"right": "TurnRight", "left": "TurnLeft", "around": "TurnAround", "random": "TurnRandom"}
      if n != WORDS_WITH_ARGUMENT or words[1] not in turns:
         raise RouteError(f"turn takes one of {', '.join(turns)}")
      return bytes([OP[turns[words[1]]]])
   if w in ("set", "clear"):
      if n != WORDS_WITH_ARGUMENT:
         raise RouteError(f"{w} takes a flag name")
      return bytes([OP["Flag"]]) + struct.pack("<HB", flag_id(words[1]), 1 if w == "set" else 0)
   if w == "speed":
      if n != WORDS_WITH_ARGUMENT:
         raise RouteError("speed takes 1-4")
      return bytes([OP["Speed"], _int(words[1], 1, MAX_SPEED, "speed")])
   if w == "frequency":
      if n != WORDS_WITH_ARGUMENT:
         raise RouteError("frequency takes 1-5")
      return bytes([OP["Frequency"], _int(words[1], 1, MAX_FREQUENCY, "frequency")])
   if w in OPTIONS:
      return bytes([OP["Option"], OPTIONS[w], _onoff(words, w)])
   if w == "sprite":
      if n != WORDS_WITH_ARGUMENT or not words[1].startswith("sprite:"):
         raise RouteError("sprite takes sprite:NAME")
      return bytes([OP["Sprite"]]) + struct.pack("<I", sprite_key(words[1]))
   if w == "in_place":
      if n not in (WORDS_WITH_ARGUMENT, WORDS_WITH_TWO_ARGUMENTS) or words[1] not in DIRECTIONS:
         raise RouteError("in_place takes a direction and a speed (1-4)")
      speed = DEFAULT_SPEED
      if n == WORDS_WITH_TWO_ARGUMENTS:
         speed = _int(words[SECOND_ARGUMENT], 1, MAX_SPEED, "in_place speed")
      return bytes([OP["InPlace"], DIRECTIONS.index(words[1]), speed])
   if w == "step":
      if n not in (WORDS_WITH_ARGUMENT, WORDS_WITH_TWO_ARGUMENTS):
         raise RouteError("step takes a game route step's name and an optional argument")
      if step_id is None:
         raise RouteError("game route steps need the scripts' step declarations")
      arg = _int(words[SECOND_ARGUMENT], 0, MAX_STEP_ARG, "step argument") if n == WORDS_WITH_TWO_ARGUMENTS else 0
      return bytes([OP["Game"], step_id(words[1]), arg])
   raise RouteError(f"unknown step {w!r}")


def compile_route(steps: Sequence[Sequence[str]], repeat: bool, skip: bool, flag_id: FlagIdFn,
                  sprite_key: SpriteKeyFn, step_id: StepIdFn | None = None) -> bytes:
   """!Compiles a route.
   @param steps       list of word lists
   @param repeat      start over at the end
   @param skip        skip blocked steps
   @param flag_id     callable(name) -> flag ID
   @param sprite_key  callable("sprite:NAME") -> asset key
   @param step_id     callable(name) -> a game route step's ID (I4), or None
   @return bytes: flags, steps, End
   """
   out = bytearray([(REPEAT if repeat else 0) | (SKIP if skip else 0)])
   moves = False
   for i, words in enumerate(steps):
      try:
         b = step(words, flag_id, sprite_key, step_id)
      except RouteError as e:
         raise RouteError(f"step {i + 1} ({' '.join(words)}): {e}") from None
      # a step that takes time keeps a repeating route from spinning in one frame
      moves = moves or b[0] in (OP["Move"], OP["Random"], OP["Toward"], OP["Away"], OP["Forward"],
                                OP["Backward"], OP["Jump"], OP["Wait"], OP["InPlace"])
      out += b
   if repeat and not moves:
      raise RouteError("a repeating route needs a step that takes time (a move, jump or wait)")
   out.append(OP["End"])
   if len(out) > MAX_BYTES:
      raise RouteError(f"route is {len(out)} bytes; at most {MAX_BYTES}")
   return bytes(out)


def parse_text(text: object) -> list[str]:
   """!Splits a JSON route step into words.
   @param text  e.g. "up 2"
   @return list of words
   """
   if not isinstance(text, str):
      raise RouteError(f"a step is a string, got {text!r}")
   return text.split()
