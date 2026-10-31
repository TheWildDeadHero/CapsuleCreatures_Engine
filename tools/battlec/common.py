"""!Shared parts of the battle compiler (tools/battle_compiler.py): the error
type, the value checks every section uses (integers, fractions, Q8, odds),
text encoding, name lookups and the registry of allowed manifest keys.
"""
from __future__ import annotations

import re
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path
from typing import Any

import text_encoder

## Placeholders -> argument byte after FD (capsule::text::Arg).
PLACEHOLDERS = dict(text_encoder.PLACEHOLDERS)
## Largest value of an 8-bit field.
U8_MAX = 0xFF
## Largest value of a 16-bit field.
U16_MAX = 0xFFFF
## Largest value of a signed 16-bit field.
I16_MAX = 0x7FFF
## A random generator's name (a game's next16() source: a bench scenario's "rng", a declared generator).
GENERATOR_NAME = re.compile(r"[a-z][a-z0-9_]*\Z")
## "None" in an 8-bit index field (capsule::battle::kNone8).
NONE8 = 0xFF
## "None" in a 16-bit index field (capsule::battle::kNone16, monster::kNoMove).
NONE16 = 0xFFFF
## Text code: continue on the next row (capsule::text::kCodeNewline).
CODE_NEWLINE = text_encoder.CODE_NEWLINE
## Text code: end of string (capsule::text::kCodeEnd).
CODE_END = text_encoder.CODE_END
## Q8 fixed point: the value of x1.
Q8_ONE = 256
## Largest Q8 value (x127.99, a signed 16-bit field).
Q8_MAX = 0x7FFF
## Halves per unit: doubling, adding one and halving again rounds to the nearest.
HALVES = 2
## Largest denominator a decimal (float) fraction is approximated with.
FLOAT_DENOMINATOR_LIMIT = 4096
## A percentage's denominator; also a neutral percent multiplier (x1).
PERCENT = 100
## Numbers in a pair ([n, m] odds, [min, max] turns, [level, move], [type, multiplier]...).
PAIR = 2
## Lowest presenter offset (int8).
I8_MIN = -0x80
## Highest presenter offset (int8).
I8_MAX = 0x7F
## A parsed JSON value.
Json = Any
## In-memory manifests replacing files (resolved Path -> parsed JSON), as editors pass them.
Sources = Mapping[Path, Json]
## Name lookups by kind ("types", "statuses", ...).
Lookups = Mapping[str, "Names"]


class ManifestError(Exception):
   """!A manifest rule was broken; the message says where and how."""
   pass


def strip(node: Json) -> Json:
   """!Drops editor-only data everywhere (P-1 rule 2).
   @param node  any JSON value
   @return a copy without "editor" and "_"-prefixed keys
   """
   if isinstance(node, dict):
      return {k: strip(v) for k, v in node.items() if k != "editor" and not k.startswith("_")}
   if isinstance(node, list):
      return [strip(v) for v in node]
   return node


def check_keys(obj: Json, kind: str, where: str) -> None:
   """!Rejects unknown keys.
   @param obj    dict
   @param kind   KEYS entry
   @param where  location for the error message
   @return None
   """
   if not isinstance(obj, dict):
      raise ManifestError(f"{where}: must be an object")
   unknown = sorted(set(obj) - KEYS[kind])
   if unknown:
      raise ManifestError(f"{where}: unknown key(s) {', '.join(unknown)}")


def integer(v: Json, lo: int, hi: int, where: str) -> int:
   """!Checks an integer in [lo, hi].
   @param v      value
   @param lo     lowest
   @param hi     highest
   @param where  location
   @return v
   """
   if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
      raise ManifestError(f"{where}: must be an integer {lo}-{hi}, got {v!r}")
   return v


def fraction(v: Json, where: str) -> tuple[int, int]:
   """!Parses a fraction ("1/8", "50%", 0.125) to (num, den).
   @param v      value
   @param where  location
   @return (num, den)
   """
   if isinstance(v, bool):
      raise ManifestError(f"{where}: expected a fraction")
   if isinstance(v, int):
      return v, 1
   if isinstance(v, float):
      from fractions import Fraction
      f = Fraction(v).limit_denominator(FLOAT_DENOMINATOR_LIMIT)
      return f.numerator, f.denominator
   if isinstance(v, str):
      m = re.fullmatch(r"\s*(?P<n>\d+)\s*/\s*(?P<d>\d+)\s*", v)
      if m and int(m.group("d")) > 0:
         return int(m.group("n")), int(m.group("d"))
      m = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*%\s*", v)
      if m:
         from fractions import Fraction
         f = Fraction(m.group(1)) / PERCENT
         return f.numerator, f.denominator
   raise ManifestError(f"{where}: expected a fraction like \"1/8\", \"30%\" or 0.125, got {v!r}")


def q8(v: Json, where: str, lo: int = 0, hi: int = Q8_MAX) -> int:
   """!A multiplier or fraction as Q8 (rounded to nearest, P-4).
   @param v      value
   @param where  location
   @param lo     lowest Q8
   @param hi     highest Q8
   @return int
   """
   n, d = fraction(v, where)

   # n/d in Q8, rounded half up: (2 * 256n + d) // 2d
   r = (n * Q8_ONE * HALVES + d) // (HALVES * d)
   if not lo <= r <= hi:
      raise ManifestError(f"{where}: {v!r} is out of range")
   return r


def odds(v: Json, where: str, max_m: int = U16_MAX) -> tuple[int, int]:
   """!Integer odds (P-5).
   @param v      "1/3", "30%", or [n, m]
   @param where  location
   @param max_m  largest denominator
   @return (n, m)
   """
   if isinstance(v, list) and len(v) == PAIR:
      vn, vm = v
      n, m = integer(vn, 0, max_m, where), integer(vm, 1, max_m, where)
   else:
      n, m = fraction(v, where)
   if m > max_m or n > m:
      raise ManifestError(f"{where}: odds {v!r} must be at most 1 with a denominator up to {max_m}")
   return n, m


def encode(value: Json, where: str, placeholders: Iterable[str] = (),
           codepage: text_encoder.CodePage = text_encoder.DEFAULT, terms: Mapping[str, Any] | None = None, *,
           field: Mapping[str, int] | None = None) -> list[int]:
   """!Encodes a string into the game's code page.
   @param value         the string
   @param where         location for error messages
   @param placeholders  placeholder names allowed here ({user} ...); empty: none
   @param codepage      the game's code page
   @param terms         terms for {@concept} (compile_terms()), or None where terms are not expanded
   @param field         the game's field placeholders ({player}: FD 80-FE, resolved when the text prints;
                        script_compiler.load_placeholders()), also allowed here, or None
   @return list of codes ending with 0xFF
   """
   allowed, known = set(placeholders), PLACEHOLDERS
   if field:
      allowed |= set(field)
      known = {**PLACEHOLDERS, **field}
   try:
      return text_encoder.encode(value, where, allowed, terms, codepage=codepage, known=known)
   except text_encoder.TextError as e:
      raise ManifestError(str(e)) from None


def ident(name: str) -> str:
   """!C++ identifier from a display name.
   @param name  any name
   @return UPPER_SNAKE
   """
   s = re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_").upper()
   return ("_" + s) if s[:1].isdigit() else (s or "_")


class Names:
   """!Name -> index lookup for one kind of entry."""

   def __init__(self, kind: str, entries: Sequence[Json], where: str, first: int = 0) -> None:
      """!Builds the lookup.
      @param kind     what they are, for messages
      @param entries  list of dicts with "name"
      @param where    location
      @param first    index of entries[0]
      """
      ## What the names are.
      self.kind = kind
      ## name -> index.
      self.index: dict[str, int] = {}
      for i, e in enumerate(entries):
         n = e.get("name") if isinstance(e, dict) else None
         if not isinstance(n, str) or not n.strip():
            raise ManifestError(f"{where}[{i}]: needs a name")
         if n in self.index:
            raise ManifestError(f"{where}[{i}]: duplicate {kind} {n!r}")
         self.index[n] = first + i

   def __call__(self, name: Json, where: str) -> int:
      """!Resolves a name.
      @param name   the name
      @param where  location
      @return index
      """
      if name not in self.index:
         raise ManifestError(f"{where}: unknown {self.kind} {name!r}")
      return self.index[name]

   def name_of(self, index: int) -> str:
      """!The name with an index.
      @param index  an index __call__ gave
      @return the name
      """
      return next(n for n, i in self.index.items() if i == index)


## Allowed keys per manifest object, filled by each battlec module for the objects it compiles (register_keys()).
KEYS: dict[str, set[str]] = {}


def register_keys(keys: Mapping[str, set[str]]) -> None:
   """!Adds the allowed keys of a module's manifest objects to KEYS (check_keys() reads them).
   @param keys  {object kind: allowed keys}
   @return None
   """
   for kind, allowed in keys.items():
      if kind in KEYS and KEYS[kind] != allowed:
         raise ValueError(f"battlec: manifest object {kind!r} registered twice with different keys")
      KEYS[kind] = set(allowed)


class Texts:
   """!The text encoders of one compile: the game's code page and the terms."""

   def __init__(self, codepage: text_encoder.CodePage, terms: Mapping[str, Any] | None = None,
                field: Mapping[str, int] | None = None) -> None:
      """!Binds a code page, terms and field placeholders.
      @param codepage  the game's code page (every text)
      @param terms     compile_terms() result (the words of each concept), or None before the terms are compiled
      @param field     the game's field placeholders (name -> argument byte), or None
      """
      ## The game's code page.
      self.codepage = codepage
      ## The words of each concept (compile_terms()), or None.
      self.terms = terms
      ## Field placeholders in-game texts may use ({player}; the printer resolves them), or None.
      self.field = field

   def text(self, value: Json, where: str, placeholders: Iterable[str] = ()) -> list[int]:
      """!encode() with this compile's code page (names: no terms).
      @param value         the string
      @param where         location for error messages
      @param placeholders  placeholder names allowed here
      @return list of codes ending with 0xFF
      """
      return encode(value, where, placeholders, self.codepage)

   def term_text(self, value: Json, where: str, placeholders: Iterable[str] = ()) -> list[int]:
      """!encode() of an in-game text that may name concepts (the terms section) and the game's field
      placeholders.
      @param value         the string
      @param where         location for error messages
      @param placeholders  placeholder names allowed here
      @return list of codes ending with 0xFF
      """
      return encode(value, where, placeholders, self.codepage, self.terms, field=self.field)
