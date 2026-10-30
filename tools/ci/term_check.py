#!/usr/bin/env python3
"""!Word denylist check for tools/ci/check_principles.sh (D-40): finds listed words in files without the repository
spelling the listed words out.

A list file (tools/ci/terms/*.sha256) holds a salt and the salted SHA-256 digests of its words:

    # comment lines
    salt 3f9c...            (hex)
    case insensitive        (or "case sensitive"; optional, insensitive by default)
    9b74c9897bac770ffc029102a200c5de...   (one digest per word or phrase)

A word is normalised before it is hashed: accented letters folded to ASCII (NFKD) and, for a case-insensitive
list, lower-cased. A phrase is its words joined by one space. The digest is sha256(salt + ":" + word) in hex.
Plain words can be given on the command line too (`--word`), for generic words that need no hiding.

Usage:

    term_check.py check [--list FILE ...] [--word WORD ...] [--tokens word|letters] [--include GLOB ...]
                        [--exclude NAME ...] [--exclude-dir NAME ...] PATH ...
    term_check.py hash --list FILE < words.txt      (prints the digests of the words read, one per line)

`check` prints `file:line:text` for each line holding a listed word, as grep does, and exits 1 when there is one
(0 otherwise). Tokens: `word` splits at characters other than letters, digits and underscores (grep -w);
`letters` splits at anything but letters (underscores and digits separate words too). Binary files are skipped.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import re
import sys
import unicodedata
from pathlib import Path

## Bytes looked at to decide whether a file is binary (grep -I's window).
BINARY_SNIFF_BYTES = 8000
## Longest phrase, in words, that a list may hold.
MAX_PHRASE_WORDS = 3
## Exit status when a listed word was found.
EXIT_FOUND = 1
## Exit status for a bad list file or bad arguments.
EXIT_USAGE = 2
## Token patterns by --tokens choice.
TOKEN_PATTERNS: dict[str, re.Pattern[str]] = {
   "word": re.compile(r"[A-Za-z0-9_]+"),
   "letters": re.compile(r"[A-Za-z]+"),
}
## Gaps between two tokens that still make a phrase (whitespace only).
PHRASE_GAP = re.compile(r"\s+")


def fold(text: str) -> str:
   """!Folds accented letters to plain ASCII (é -> e), dropping what has no ASCII form.
   @param text  any text
   @return the folded text
   """
   return unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode("ascii")


def digest(salt: str, word: str) -> str:
   """!The salted digest of one normalised word or phrase.
   @param salt  the list's salt
   @param word  the normalised word
   @return hex SHA-256
   """
   return hashlib.sha256(f"{salt}:{word}".encode("ascii")).hexdigest()


class TermList:
   """!One list file: its salt, its case rule and its digests."""

   def __init__(self, salt: str, case_sensitive: bool, digests: set[str]) -> None:
      """!Keeps a list's parts.
      @param salt            the salt every digest was made with
      @param case_sensitive  whether words keep their case before hashing
      @param digests         the digests of the listed words and phrases
      """
      ## The salt every digest of the list was made with.
      self.salt = salt
      ## Whether the list keeps the words' case (else they are lower-cased before hashing).
      self.case_sensitive = case_sensitive
      ## The digests of the listed words and phrases.
      self.digests = digests

   def normalise(self, word: str) -> str:
      """!A word as this list hashes it.
      @param word  the word or phrase as written
      @return the normalised word
      """
      folded = fold(word)
      return folded if self.case_sensitive else folded.lower()

   def has(self, word: str) -> bool:
      """!Whether a word or phrase is listed.
      @param word  the word or phrase as written
      @return True when its digest is in the list
      """
      return digest(self.salt, self.normalise(word)) in self.digests


def load_list(path: Path) -> TermList:
   """!Reads a list file.
   @param path  the list file
   @return the list
   """
   salt = ""
   case_sensitive = False
   digests: set[str] = set()
   for raw in path.read_text(encoding = "ascii").splitlines():
      line = raw.strip()
      if not line or line.startswith("#"):
         continue
      key, _, value = line.partition(" ")
      if key == "salt":
         salt = value.strip()
      elif key == "case":
         case_sensitive = value.strip() == "sensitive"
      else:
         digests.add(line.lower())
   if not salt:
      sys.exit(f"{path}: no salt line")
   return TermList(salt, case_sensitive, digests)


class Matcher:
   """!The hashed lists and the plain words of one check."""

   def __init__(self, lists: list[TermList], plain: set[str], tokens: re.Pattern[str]) -> None:
      """!Keeps a check's lists and words.
      @param lists   the hashed lists
      @param plain   the plain words and phrases (folded, lower case)
      @param tokens  what a word is (TOKEN_PATTERNS)
      """
      ## The hashed lists.
      self.lists = lists
      ## The plain words and phrases (folded, lower case).
      self.plain = plain
      ## What a word is (TOKEN_PATTERNS).
      self.tokens = tokens

   def listed(self, word: str) -> bool:
      """!Whether a word or phrase is in any list or among the plain words (plain words: any case).
      @param word  the word or phrase as written
      @return True when it is listed
      """
      return fold(word).lower() in self.plain or any(t.has(word) for t in self.lists)

   def line_hits(self, line: str) -> bool:
      """!Whether a line holds a listed word, or a listed phrase of up to MAX_PHRASE_WORDS words.
      @param line  one line of text
      @return True on a hit
      """
      found = list(self.tokens.finditer(fold(line)))
      for index, match in enumerate(found):
         phrase = match.group(0)
         if self.listed(phrase):
            return True
         for width in range(1, MAX_PHRASE_WORDS):
            if index + width >= len(found):
               break
            nxt = found[index + width]
            gap = fold(line)[found[index + width - 1].end() : nxt.start()]
            if not PHRASE_GAP.fullmatch(gap):
               break
            phrase = f"{phrase} {nxt.group(0)}"
            if self.listed(phrase):
               return True
      return False


def wanted(path: Path, includes: list[str], excludes: list[str]) -> bool:
   """!Whether a file's name passes --include / --exclude.
   @param path      the file
   @param includes  name globs (none: every file)
   @param excludes  file names left out
   @return True to scan it
   """
   if path.name in excludes:
      return False
   return not includes or any(fnmatch.fnmatch(path.name, g) for g in includes)


def files_under(roots: list[str], includes: list[str], excludes: list[str], exclude_dirs: list[str]) -> list[Path]:
   """!The files to scan, in a stable order (missing paths are skipped, as nothing to scan).
   @param roots         files or folders
   @param includes      name globs
   @param excludes      file names left out
   @param exclude_dirs  folder names left out
   @return paths
   """
   out: list[Path] = []
   for root in roots:
      base = Path(root)
      if base.is_file():
         if wanted(base, includes, excludes):
            out.append(base)
         continue
      if not base.is_dir():
         continue
      for path in sorted(base.rglob("*")):
         if path.is_file() and not any(part in exclude_dirs for part in path.relative_to(base).parts[: -1]) and \
               wanted(path, includes, excludes):
            out.append(path)
   return out


def check(args: argparse.Namespace) -> int:
   """!Runs `check`: prints each line with a listed word.
   @param args  parsed arguments
   @return exit status
   """
   matcher = Matcher([load_list(Path(p)) for p in args.list], {fold(w).lower() for w in args.word},
                     TOKEN_PATTERNS[args.tokens])
   status = 0
   for path in files_under(args.paths, args.include, args.exclude, args.exclude_dir):
      data = path.read_bytes()
      if b"\0" in data[: BINARY_SNIFF_BYTES]:
         continue
      for number, line in enumerate(data.decode("utf-8", errors = "replace").splitlines(), start = 1):
         if matcher.line_hits(line):
            print(f"{path}:{number}:{line}")
            status = EXIT_FOUND
   return status


def hash_words(args: argparse.Namespace) -> int:
   """!Runs `hash`: prints the digest of every word read from standard input (one word or phrase per line).
   @param args  parsed arguments (one --list: its salt and case rule are used)
   @return exit status
   """
   if len(args.list) != 1:
      print("hash: give exactly one --list", file = sys.stderr)
      return EXIT_USAGE
   terms = load_list(Path(args.list[0]))
   for line in sys.stdin.read().splitlines():
      word = " ".join(line.split())
      if word:
         print(digest(terms.salt, terms.normalise(word)))
   return 0


def main() -> int:
   """!Parses the command line and runs the command.
   @return exit status
   """
   parser = argparse.ArgumentParser(description = "Hashed word denylists (tools/ci/terms/).")
   parser.add_argument("command", choices = ("check", "hash"))
   parser.add_argument("--list", action = "append", default = [], help = "a list file (repeatable)")
   parser.add_argument("--word", action = "append", default = [], help = "a plain word (repeatable)")
   parser.add_argument("--tokens", choices = sorted(TOKEN_PATTERNS), default = "word")
   parser.add_argument("--include", action = "append", default = [], help = "file name glob (repeatable)")
   parser.add_argument("--exclude", action = "append", default = [], help = "file name left out (repeatable)")
   parser.add_argument("--exclude-dir", action = "append", default = [], help = "folder name left out (repeatable)")
   parser.add_argument("paths", nargs = "*")
   args = parser.parse_args()
   if args.command == "hash":
      return hash_words(args)
   return check(args)


if __name__ == "__main__":
   sys.exit(main())
