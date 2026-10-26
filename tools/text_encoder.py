#!/usr/bin/env python3
"""!Shared text encoder: Unicode text to a per-language 8-bit code page (C-2, D-20).

Every compiler that writes text (assets, scripts, location names, battle data)
encodes it here, with the code page of the game's language. A code page is a
manifest (capsule.codepage/1) that gives each character its code:

    {"schema": "capsule.codepage/1", "id": "en", "name": "English (ASCII)",
     "ranges": [{"first": "0x20", "chars": " !\\"#$%..."}],
     "map": {"0x80": "é"},
     "aliases": {"’": "'"},
     "names": {"heart": ["0x8A"], "key_icon": ["0x8B", "0x8C"]}}

- ranges: consecutive codes from "first", one code point of "chars" each;
- map: single codes, each a character (one or more code points, such as a
  letter with a combining mark);
- aliases: explicit folds of other text onto mapped characters (never
  implicit: a character neither mapped nor aliased is an error);
- names (I4): named glyphs, `{#name}` in markup: the codes of glyphs that are
  no character (ligature halves, icons, superscripts); a code a name uses
  without a character still has a glyph in the page's fonts.

Rules: codes 0x00-0xF9 (kMaxGlyphCode), each code once, each character once;
space must be 0x20 and the digits '0'-'9' 0x30-0x39 (the engine prints blanks
and numbers with them). Text is NFC-normalised and matched longest first.

Code bytes (capsule/text/encode.h): 0x00-0xF9 glyphs; FA prompt (wait for a
button, then go on); FB page break (wait for a button, then clear the box);
FC nn control (unknown controls are skipped at run time); FD nn placeholder;
FE newline; FF end.

Placeholder arguments (the nn after FD) come in two ranges that never meet:

| nn | Placeholders | Replaced by |
|---|---|---|
| 0x00-0x7F | battle (PLACEHOLDERS, capsule::text::Arg) | battle::format() |
| 0x80-0xFE | field (a game's declared names, field_placeholders()) | the printers, through the field resolver |

0xFF is never an argument: a printer that reads FD FF stops at the end code.

Markup (when allowed): `{name}` is a placeholder (FD nn) where the caller
allows it (I4: a name may have one `:` part, e.g. `{str:1}`); `{prompt}`
(FA) and `{page}` (FB) where the caller allows controls, and there too (I4)
the generic controls `{control}` / `{control:a,b}` (CONTROL_CODES: FC, the
control's byte, then each argument in its bytes, little-endian), e.g.
`{pause:15}`, `{color:4}`, `{se:21}`, `{scroll}` (wait for a button, scroll
the box up one line and go on at the start of its last line); `{#name}` is a
named glyph of the page; `{@concept}` and `{@concept:many}` expand a term
from the caller's terms mapping before anything else is encoded.

The engine's default page is assets/text/ascii.codepage.json (DEFAULT).
"""
from __future__ import annotations

import json
import re
import unicodedata
from collections.abc import Iterable, Mapping
from os import PathLike
from pathlib import Path
from typing import Any

## Manifest schema of a code page.
SCHEMA = "capsule.codepage/1"
## Keys a code page may have.
KEYS = {"schema", "id", "name", "ranges", "map", "aliases", "names", "editor"}
## Keys of one range.
RANGE_KEYS = {"first", "chars"}

## Highest glyph code (capsule::text::kMaxGlyphCode).
MAX_GLYPH_CODE = 0xF9
## Most glyphs a font holds: codes 0 to MAX_GLYPH_CODE (capsule::text::kMaxFontGlyphs).
MAX_FONT_GLYPHS = MAX_GLYPH_CODE + 1
## Reserved: wait for a button, then continue (capsule::text::kCodePrompt).
CODE_PROMPT = 0xFA
## Reserved: wait for a button, then clear the box (capsule::text::kCodePageBreak).
CODE_PAGE_BREAK = 0xFB
## Control escape: the next byte names the control (capsule::text::kCodeControl).
CODE_CONTROL = 0xFC
## Placeholder escape: the next byte names the argument (capsule::text::kCodePlaceholder).
CODE_PLACEHOLDER = 0xFD
## Continue on the next row (capsule::text::kCodeNewline).
CODE_NEWLINE = 0xFE
## End of text (capsule::text::kCodeEnd).
CODE_END = 0xFF
## Code every page gives the space (capsule::text::kCodeSpace).
CODE_SPACE = 0x20
## Code every page gives the digit '0'; '1'-'9' follow it (capsule::text::kCodeDigitZero).
CODE_DIGIT_ZERO = 0x30
## Digits '0'-'9'.
DIGITS = "0123456789"

## Placeholder names -> argument byte after FD (capsule::text::Arg).
PLACEHOLDERS = {"user": 0, "target": 1, "move": 2, "value": 3, "stat": 4, "party": 5, "passive": 6, "item": 7,
                "status": 8}
## Highest battle placeholder argument (capsule::text::kLastBattleArg); battle arguments start at 0.
LAST_BATTLE_ARG = 0x7F
## First field placeholder argument (capsule::text::kFirstFieldArg): field placeholder i is FD (0x80 + i).
FIRST_FIELD_ARG = 0x80
## Highest field placeholder argument (capsule::text::kLastFieldArg; 0xFF would read as the end).
LAST_FIELD_ARG = 0xFE
## Most field placeholders a game declares (capsule::text::kMaxFieldArgs).
MAX_FIELD_PLACEHOLDERS = LAST_FIELD_ARG - FIRST_FIELD_ARG + 1
## Most codes one field placeholder expands to (capsule::text::kMaxPlaceholderCodes).
MAX_PLACEHOLDER_CODES = 24
## Control markup names -> their code, for texts that allow controls (field texts).
CONTROLS = {"prompt": CODE_PROMPT, "page": CODE_PAGE_BREAK}
## Codes of a control escape before its arguments (FC and the control's byte; I4).
CONTROL_HEAD = 2
## Codes of a placeholder escape (FD and its argument).
PLACEHOLDER_CODES = 2
## I4: the generic controls (FC nn, capsule::text::Control): name -> (control byte, the byte width of each
## argument). Printers that do not know a control skip it with its arguments (capsule::text::escape_length()).
CONTROL_CODES: dict[str, tuple[int, tuple[int, ...]]] = {
   "color": (0x01, (1,)), "highlight": (0x02, (1,)), "shadow": (0x03, (1,)), "colors": (0x04, (1, 1, 1)),
   "palette": (0x05, (1,)), "font": (0x06, (1,)), "reset_font": (0x07, ()), "pause": (0x08, (1,)),
   "wait_button": (0x09, ()), "wait_se": (0x0A, ()), "bgm": (0x0B, (2,)), "escape": (0x0C, (1,)),
   "shift_right": (0x0D, (1,)), "shift_down": (0x0E, (1,)), "fill": (0x0F, ()), "se": (0x10, (2,)),
   "clear": (0x11, (1,)), "skip_to": (0x12, (1,)), "clear_to": (0x13, (1,)), "min_spacing": (0x14, (1,)),
   "lang_jp": (0x15, ()), "lang_en": (0x16, ()), "pause_music": (0x17, ()), "resume_music": (0x18, ()),
   "scroll": (0x19, ()), "icon": (0x1A, (1,)), "name_end": (0x1B, ()), "dynamic": (0x1C, (1,))}
## Separator of a control's arguments in markup.
ARG_SEPARATOR = ","
## Prefix of a named glyph in markup (I4).
GLYPH_PREFIX = "#"
## Pattern of a placeholder name: a lower-case identifier (I4: with an optional `:` part, e.g. str:1).
NAME_PATTERN = re.compile(r"[a-z_][a-z0-9_]*(:[a-z0-9_]+)?\Z")
## Pattern of a glyph name.
GLYPH_NAME_PATTERN = re.compile(r"[a-z_][a-z0-9_]*\Z")
## Bits of an argument byte.
BYTE_BITS = 8
## Mask of an argument byte.
BYTE_MASK = 0xFF

## Most glyphs a font may have when text goes on BG3: char block 3's text tiles start at 160
## (gfx::background::kTextTiles + 16) and its tiles 256 and up hold screen blocks 28-31.
BG3_MAX_FONT_GLYPHS = 96

## Start of a markup item.
MARKUP_OPEN = "{"
## End of a markup item.
MARKUP_CLOSE = "}"
## Prefix of a term in markup.
TERM_PREFIX = "@"
## Separator of a term's form.
TERM_FORM_SEPARATOR = ":"
## Term forms: the one form, used when no form is named, and the many form.
TERM_FORMS = ("one", "many")
## Prefix of hex codes in a manifest.
HEX_PREFIX = "0x"
## Base of hex codes.
HEX_BASE = 16
## Shown by decode() for a glyph code the page leaves unmapped.
UNMAPPED_CHAR = "\ufffd"
## Unicode normal form of all text (composed).
NORMAL_FORM = "NFC"

## Words of a command-line option with its value.
OPTION_WORDS = 2
## Path of the engine's default code page.
DEFAULT_PATH = Path(__file__).resolve().parents[1] / "assets" / "text" / "ascii.codepage.json"


class TextError(ValueError):
   """!A text the code page cannot encode, or an invalid code page."""


def _code(value: Any, where: str) -> int:
   """!A code from a manifest: "0x41" or an integer.
   @param value  the value
   @param where  location for the error message
   @return the code
   """
   code = None
   if isinstance(value, int) and not isinstance(value, bool):
      code = value
   elif isinstance(value, str) and value.lower().startswith(HEX_PREFIX):
      try:
         code = int(value, HEX_BASE)
      except ValueError:
         code = None
   if code is None or not 0 <= code <= MAX_GLYPH_CODE:
      raise TextError(f"{where}: code {value!r} must be 0x00-0x{MAX_GLYPH_CODE:02X} (\"0x..\" or a number)")
   return code


def _character(value: Any, where: str) -> str:
   """!A mapped character: a non-empty NFC string without a newline.
   @param value  the value
   @param where  location for the error message
   @return the character
   """
   if not isinstance(value, str) or not value:
      raise TextError(f"{where}: a character must be a non-empty string")
   if unicodedata.normalize(NORMAL_FORM, value) != value:
      raise TextError(f"{where}: character {value!r} is not NFC-normalised")
   if "\n" in value:
      raise TextError(f"{where}: the newline is not a character (it is always 0x{CODE_NEWLINE:02X})")
   return value


class CodePage:
   """!One language's code page: characters <-> 8-bit glyph codes."""

   def __init__(self, data: Mapping[str, Any], where: str = "code page") -> None:
      """!Checks and indexes a parsed capsule.codepage/1 manifest.
      @param data   the parsed JSON
      @param where  name for error messages (the file)
      """
      if not isinstance(data, Mapping):
         raise TextError(f"{where}: a code page is a JSON object")
      unknown = sorted(set(data) - KEYS)
      if unknown:
         raise TextError(f"{where}: unknown keys {unknown}")
      if data.get("schema") != SCHEMA:
         raise TextError(f"{where}: schema must be {SCHEMA!r}")
      if not isinstance(data.get("id"), str) or not data["id"]:
         raise TextError(f"{where}: needs an id (the language)")
      ## Language id (project.json text.language).
      self.id = data["id"]
      ## Display name.
      self.name = str(data.get("name", self.id))
      ## Where it came from, for messages.
      self.where = where
      ## The file it was read from (None for a page built in memory).
      self.path = None
      ## Code -> character (also the decode table).
      self.chars = {}
      ## Text -> codes it encodes to: mapped characters and aliases.
      self.table = {}
      ranges = data.get("ranges", [])
      if not isinstance(ranges, list):
         raise TextError(f"{where}: ranges is a list")
      for i, r in enumerate(ranges):
         rw = f"{where}: ranges[{i}]"
         if not isinstance(r, Mapping) or set(r) - RANGE_KEYS or not isinstance(r.get("chars"), str):
            raise TextError(f"{rw}: {{\"first\": \"0x..\", \"chars\": \"...\"}}")
         first = _code(r.get("first"), rw)
         for k, ch in enumerate(r["chars"]):
            self._add(_code(first + k, f"{rw} character {k}"), _character(ch, rw), rw)
      single = data.get("map", {})
      if not isinstance(single, Mapping):
         raise TextError(f"{where}: map is an object of code -> character")
      for code, ch in single.items():
         self._add(_code(code, f"{where}: map"), _character(ch, f"{where}: map {code}"), f"{where}: map {code}")
      if self.table.get(" ") != [CODE_SPACE]:
         raise TextError(f"{where}: the space must be code 0x{CODE_SPACE:02X}")
      for k, digit in enumerate(DIGITS):
         if self.table.get(digit) != [CODE_DIGIT_ZERO + k]:
            raise TextError(f"{where}: the digit {digit!r} must be code 0x{CODE_DIGIT_ZERO + k:02X}")
      ## Longest text in the table (longest match first).
      self.longest = max(len(t) for t in self.table)
      aliases = data.get("aliases", {})
      if not isinstance(aliases, Mapping):
         raise TextError(f"{where}: aliases is an object of text -> mapped text")
      folded = {}
      mapped = self.table   # a local alias (Doxygen 1.12 reports `x in self.table` as an undocumented member)
      for src, dst in aliases.items():
         aw = f"{where}: alias {src!r}"
         src = _character(src, aw)
         if src in mapped:
            raise TextError(f"{aw}: the character is mapped itself")
         codes = self._match_all(_character(dst, aw), aw)
         folded[src] = codes
      self.table.update(folded)
      longest = self.longest
      self.longest = max([longest] + [len(t) for t in folded])
      ## I4: named glyphs: name -> codes (`{#name}`).
      self.names = {}
      named = data.get("names", {})
      if not isinstance(named, Mapping):
         raise TextError(f"{where}: names is an object of name -> [codes]")
      for name, codes in named.items():
         nw = f"{where}: name {name!r}"
         if not isinstance(name, str) or not GLYPH_NAME_PATTERN.match(name):
            raise TextError(f"{nw}: a glyph name is a lower-case identifier")
         if not isinstance(codes, list) or not codes:
            raise TextError(f"{nw}: a list of codes")
         self.names[name] = [_code(c, nw) for c in codes]

   def _add(self, code: int, ch: str, where: str) -> None:
      """!Maps one character.
      @param code   its code
      @param ch     the character
      @param where  location for error messages
      @return None
      """
      if code in self.chars:
         raise TextError(f"{where}: code 0x{code:02X} is given twice ({self.chars[code]!r} and {ch!r})")
      if ch in self.table:
         raise TextError(f"{where}: character {ch!r} is given twice (0x{self.table[ch][0]:02X} and 0x{code:02X})")
      self.chars[code] = ch
      self.table[ch] = [code]

   @classmethod
   def load(cls, path: str | PathLike[str]) -> CodePage:
      """!Reads a code page file.
      @param path  the .codepage.json file
      @return the code page
      """
      path = Path(path)
      try:
         data = json.loads(path.read_text(encoding="utf-8"))
      except OSError as e:
         raise TextError(f"{path}: cannot read the code page ({e.strerror})") from None
      except json.JSONDecodeError as e:
         raise TextError(f"{path}: invalid JSON ({e})") from None
      page = cls(data, path.name)
      page.path = path.resolve()
      return page

   def _match(self, text: str, i: int) -> tuple[list[int], int] | None:
      """!The longest table entry at a position.
      @param text  NFC text
      @param i     position
      @return (codes, length), or None when nothing matches
      """
      for n in range(min(self.longest, len(text) - i), 0, -1):
         codes = self.table.get(text[i:i + n])
         if codes is not None:
            return codes, n
      return None

   def _match_all(self, text: str, where: str) -> list[int]:
      """!Glyph codes of plain text (no newline, no markup), every character mapped.
      @param text   NFC text
      @param where  location for the error message
      @return the codes
      """
      out = []
      i = 0
      while i < len(text):
         m = self._match(text, i)
         if m is None:
            raise TextError(f"{where}: character {_grapheme(text, i)!r} is not in the code page {self.id!r}")
         out += m[0]
         i += m[1]
      return out

   def glyph_codes(self) -> list[int]:
      """!The codes the page maps to characters (the glyphs its font must have).
      @return sorted codes
      """
      return sorted(self.chars)

   def named_codes(self) -> list[int]:
      """!The codes of named glyphs that no character has (I4; a fixed-cell font keeps them blank).
      @return sorted codes
      """
      return sorted({c for codes in self.names.values() for c in codes} - set(self.chars))

   def font_span(self) -> tuple[int, int]:
      """!The glyph range a font for this page covers (lowest code to highest, gaps blank; named glyphs included).
      @return (lowest code, glyph count)
      """
      codes = sorted(set(self.glyph_codes()) | set(self.named_codes()))
      return codes[0], codes[-1] - codes[0] + 1

   def missing(self, text: str) -> list[str]:
      """!Characters of a text this page cannot encode (newlines and braces count as encodable).
      @param text  any text
      @return the distinct unencodable characters, in order of first appearance
      """
      text = unicodedata.normalize(NORMAL_FORM, text)
      out = []
      i = 0
      while i < len(text):
         if text[i] in ("\n", MARKUP_OPEN, MARKUP_CLOSE):
            i += 1
            continue
         m = self._match(text, i)
         if m is not None:
            i += m[1]
            continue
         g = _grapheme(text, i)
         if g not in out:
            out.append(g)
         i += len(g)
      return out

   def encode(self, text: Any, where: str, placeholders: Iterable[str] = (),
              terms: Mapping[str, Any] | None = None, *, known: Mapping[str, int] = PLACEHOLDERS,
              markup: bool = True, controls: bool = False) -> list[int]:
      """!Encodes a text: terms, then placeholders (FD nn), controls, glyphs, newlines (FE) and the end (FF).
      @param text          the text
      @param where         location for error messages
      @param placeholders  placeholder names allowed here; empty: none
      @param terms         {concept: text, or {"one": text, "many": text}} for {@concept}, or None
      @param known         every placeholder name -> argument byte (for "not available here" messages)
      @param markup        False: braces are plain characters (no placeholders, no terms, no controls)
      @param controls      {prompt} (FA) and {page} (FB) are allowed (field texts)
      @return the codes, ending with CODE_END
      """
      if not isinstance(text, str):
         raise TextError(f"{where}: text must be a string")
      text = unicodedata.normalize(NORMAL_FORM, text)
      if markup:
         text = expand_terms(text, where, terms)
      allowed = set(placeholders)
      out = []
      i = 0
      while i < len(text):
         ch = text[i]
         if ch == MARKUP_OPEN and markup:
            end = text.find(MARKUP_CLOSE, i)
            name = text[i + 1:end] if end > 0 else ""
            if controls and name in CONTROLS:
               out.append(CONTROLS[name])
               i = end + 1
               continue
            if name.startswith(GLYPH_PREFIX):
               # I4: a named glyph
               glyph = name[len(GLYPH_PREFIX):]
               if glyph not in self.names:
                  raise TextError(f"{where}: unknown glyph {{{name}}} (the code page {self.id!r} names "
                                  f"{', '.join(sorted(self.names)) or 'none'})")
               out += self.names[glyph]
               i = end + 1
               continue
            base, _, args = name.partition(TERM_FORM_SEPARATOR)
            if controls and base in CONTROL_CODES and name not in known:
               out += control_codes(base, args, where)   # I4: a generic control
               i = end + 1
               continue
            if name not in allowed or name not in known:
               listed = ", ".join(MARKUP_OPEN + x + MARKUP_CLOSE for x in sorted(allowed))
               what = "placeholder" if name in known else "unknown placeholder"
               raise TextError(f"{where}: {what} {{{name}}} is not available here"
                               + (f" (available: {listed})" if listed else " (no placeholders here)"))
            out += [CODE_PLACEHOLDER, known[name]]
            i = end + 1
            continue
         if ch == "\n":
            out.append(CODE_NEWLINE)
            i += 1
            continue
         m = self._match(text, i)
         if m is None:
            raise TextError(f"{where}: character {_grapheme(text, i)!r} is not in the code page {self.id!r}")
         out += m[0]
         i += m[1]
      return out + [CODE_END]

   def decode(self, codes: Iterable[int], field: Mapping[str, int] | None = None) -> str:
      """!Text of codes, for previews: placeholders as {name}, {prompt} and {page}, control escapes dropped.
      @param codes  codes (stops at CODE_END)
      @param field  the game's field placeholders (name -> argument byte), or None: they show as {80}, ...
      @return the text
      """
      names = {v: k for k, v in PLACEHOLDERS.items()}
      names.update({v: k for k, v in (field or {}).items()})
      control_names = {v: k for k, v in CONTROLS.items()}
      out = []
      it = iter(codes)
      for c in it:
         if c == CODE_END:
            break
         if c == CODE_NEWLINE:
            out.append("\n")
         elif c == CODE_PLACEHOLDER:
            arg = next(it, CODE_END)
            out.append(MARKUP_OPEN + names.get(arg, f"{arg:02X}") + MARKUP_CLOSE)
         elif c == CODE_CONTROL:
            ctl = next(it, CODE_END)
            for code, widths in CONTROL_CODES.values():
               if code == ctl:
                  for _ in range(sum(widths)):
                     next(it, CODE_END)
         elif c in control_names:
            out.append(MARKUP_OPEN + control_names[c] + MARKUP_CLOSE)
         elif c <= MAX_GLYPH_CODE:
            out.append(self.chars.get(c, UNMAPPED_CHAR))
      return "".join(out)


def control_codes(name: str, args: str, where: str) -> list[int]:
   """!A generic control's codes (I4): FC, its byte, its arguments (each in its width, little-endian).
   @param name   the control (CONTROL_CODES)
   @param args   its arguments as written ("15", "2,1,3"; "" for none)
   @param where  location for error messages
   @return the codes
   """
   code, widths = CONTROL_CODES[name]
   values = [a.strip() for a in args.split(ARG_SEPARATOR)] if args else []
   if len(values) != len(widths):
      raise TextError(f"{where}: {{{name}}} takes {len(widths)} argument(s)")
   out = [CODE_CONTROL, code]
   for v, width in zip(values, widths):
      try:
         n = int(v, 0)
      except ValueError:
         raise TextError(f"{where}: {{{name}}}: {v!r} is not a number") from None
      if not 0 <= n < 1 << (BYTE_BITS * width):
         raise TextError(f"{where}: {{{name}}}: {n} does not fit {width} byte(s)")
      for b in range(width):
         byte = n >> (BYTE_BITS * b) & BYTE_MASK
         if byte == CODE_END:
            raise TextError(f"{where}: {{{name}}}: an argument byte of 0xFF would read as the end of the text")
         out.append(byte)
   return out


def control_length(codes: list[int], i: int) -> int:
   """!How many codes a control escape at a position takes (FC, its byte and its arguments; I4).
   @param codes  the codes
   @param i      the position of FC
   @return its length (CONTROL_HEAD for a control no CONTROL_CODES entry has)
   """
   if i + 1 >= len(codes):
      return 1
   for code, widths in CONTROL_CODES.values():
      if code == codes[i + 1]:
         return CONTROL_HEAD + sum(widths)
   return CONTROL_HEAD


def _grapheme(text: str, i: int) -> str:
   """!The character at a position with the combining marks that follow it.
   @param text  NFC text
   @param i     position
   @return the character
   """
   j = i + 1
   while j < len(text) and unicodedata.combining(text[j]):
      j += 1
   return text[i:j]


def expand_terms(text: str, where: str, terms: Mapping[str, Any] | None) -> str:
   """!Replaces {@concept} and {@concept:many} with the term's text.
   @param text   NFC text
   @param where  location for error messages
   @param terms  {concept: text, or {"one": text, "many": text}}, or None (no terms)
   @return the text with every term expanded
   """
   head = MARKUP_OPEN + TERM_PREFIX
   out = []
   i = 0
   while True:
      start = text.find(head, i)
      if start < 0:
         out.append(text[i:])
         return "".join(out)
      end = text.find(MARKUP_CLOSE, start)
      if end < 0:
         raise TextError(f"{where}: term {text[start:]!r} has no closing brace")
      concept, _, form = text[start + len(head):end].partition(TERM_FORM_SEPARATOR)
      form = form or TERM_FORMS[0]
      term = (terms or {}).get(concept)
      if term is None:
         raise TextError(f"{where}: unknown term {{@{concept}}}" + ("" if terms else " (no terms here)"))
      if form not in TERM_FORMS:
         raise TextError(f"{where}: term {{@{concept}:{form}}}: the form is one of {', '.join(TERM_FORMS)}")
      value = term if isinstance(term, str) else (term.get(form) if isinstance(term, Mapping) else None)
      if isinstance(term, str) and form != TERM_FORMS[0]:
         value = None
      if not isinstance(value, str):
         raise TextError(f"{where}: term {{@{concept}}} has no {form!r} form")
      out += [text[i:start], unicodedata.normalize(NORMAL_FORM, value)]
      i = end + 1


## The engine's default code page (printable ASCII).
DEFAULT = CodePage.load(DEFAULT_PATH)


def load(path: str | PathLike[str] | None) -> CodePage:
   """!A code page from a --code-page option.
   @param path  the file, or None for the engine's default
   @return the code page
   """
   return DEFAULT if path is None else CodePage.load(path)


def option(args: list[str]) -> CodePage:
   """!Takes --code-page PATH out of a command line.
   @param args  arguments (the option and its value are removed)
   @return the code page it names, or DEFAULT
   """
   if "--code-page" not in args:
      return DEFAULT
   i = args.index("--code-page")
   if i + 1 >= len(args):
      raise TextError("--code-page needs a path")
   path = args[i + 1]
   del args[i:i + OPTION_WORDS]
   return CodePage.load(path)


def encode(text: Any, where: str, placeholders: Iterable[str] = (), terms: Mapping[str, Any] | None = None, *,
           codepage: CodePage = DEFAULT, known: Mapping[str, int] = PLACEHOLDERS, markup: bool = True,
           controls: bool = False) -> list[int]:
   """!Encodes a text with a code page (see CodePage.encode()).
   @param text          the text
   @param where         location for error messages
   @param placeholders  placeholder names allowed here
   @param terms         terms for {@concept}, or None
   @param codepage      the code page
   @param known         every placeholder name -> argument byte
   @param markup        False: braces are plain characters
   @param controls      {prompt} and {page} are allowed
   @return the codes, ending with CODE_END
   """
   return codepage.encode(text, where, placeholders, terms, known=known, markup=markup, controls=controls)


def field_placeholders(names: Iterable[str], where: str = "placeholders") -> dict[str, int]:
   """!Argument bytes of a game's field placeholders, in declaration order: FIRST_FIELD_ARG, then up.
   @param names  the declared names (lower-case identifiers; not a control name)
   @param where  location for error messages
   @return {name: argument byte}
   """
   out = {}
   for name in names:
      if not isinstance(name, str) or not NAME_PATTERN.match(name):
         raise TextError(f"{where}: placeholder name {name!r} must be a lower-case identifier")
      if name in CONTROLS or name in CONTROL_CODES:
         raise TextError(f"{where}: {{{name}}} is a control ({', '.join(sorted(CONTROLS) + sorted(CONTROL_CODES))} "
                         f"are reserved)")
      if name in out:
         raise TextError(f"{where}: placeholder {{{name}}} is declared twice")
      if len(out) == MAX_FIELD_PLACEHOLDERS:
         raise TextError(f"{where}: at most {MAX_FIELD_PLACEHOLDERS} field placeholders")
      out[name] = FIRST_FIELD_ARG + len(out)
   return out


def encode_field(text: Any, where: str, field: Mapping[str, int], terms: Mapping[str, Any] | None = None, *,
                 codepage: CodePage = DEFAULT) -> list[int]:
   """!Encodes a field text (script messages, location names) of a game that declares field placeholders:
   markup on, its placeholders and the controls {prompt} / {page} allowed.
   @param text      the text
   @param where     location for error messages
   @param field     the game's field placeholders (field_placeholders())
   @param terms     terms for {@concept}, or None
   @param codepage  the code page
   @return the codes, ending with CODE_END
   """
   return codepage.encode(text, where, field, terms, known=field, markup=True, controls=True)
