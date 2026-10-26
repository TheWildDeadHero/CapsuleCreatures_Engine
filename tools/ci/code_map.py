#!/usr/bin/env python3
"""!Builds, checks and searches the repository's code map, CODE_MAP.md (standard library only).

The map lists every source folder, every file with its one-line purpose, and the symbols each file defines, grouped
by kind (namespace, class, struct, union, enum, type alias, function, method, constant). It holds no line numbers, so
it changes only when a file or symbol is added, removed or renamed, not on every edit; `find` gives the exact line
live. Purposes come from the code: a Python module's docstring, a C/C++ file's `@file` / `@brief` header (else its
first comment). A file without one shows no purpose, which also flags a missing file header.

Usage (the root defaults to the nearest folder above this script that holds `.git`):

   code_map.py build [--root DIR]     writes CODE_MAP.md at the root
   code_map.py check [--root DIR]     exits 1 when CODE_MAP.md is out of date
   code_map.py find NAME [--kind KIND] [--exact] [--json] [--root DIR]
                                      prints `path:line kind name` for each matching symbol

Python is read with `ast`. C/C++ is read with line patterns that fit the project's style (Allman braces, one
declaration per line), tracking brace scopes so function bodies, locals and comments are skipped. It is not a full
C++ parser: declarations made by macros, or split oddly across lines, may be missed.

The same tool lives in capsule-engine (tools/ci/code_map.py) and capsule-studio (toolkit/code_map.py); keep the two
copies identical.
"""
from __future__ import annotations

import argparse
import ast
import difflib
import json
import os
import re
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path

## Name of the generated map at the repository root.
MAP_NAME = "CODE_MAP.md"
## First line of the map (also how `check` recognizes it).
MAP_TITLE = "# Code map"
## Folder names never read (version control, builds, environments, generated and third-party code).
SKIPPED_DIRS = {".git", ".hg", ".svn", ".github", ".venv", "venv", "__pycache__", "node_modules", "build",
                "build-docs", "generated", "ref_submodules", "submodules", "third_party", "thirdparty",
                "third-party", "vendor", "external", "wheels", "profiles", "_to_delete"}
## Folder name prefixes never read (build trees such as build-gba or cmake-build-debug).
SKIPPED_DIR_PREFIXES = ("build-", "build_", "cmake-build")
## C/C++ source suffixes.
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl"}
## Python source suffix.
PYTHON_SUFFIX = ".py"
## Lines of difference `check` prints at most.
MAX_DIFF_LINES = 40
## Length of a two-character comment marker ("//", "/*", "*/") or escape ("\\n").
MARKER_LENGTH = 2
## Indent of `find --json` output.
JSON_INDENT = 2
## Lines one declaration may span before it is read anyway (stops an unbalanced "(" from swallowing a file).
MAX_STATEMENT_LINES = 32

## Symbol kinds, in the order the map lists them.
KIND_ORDER = ("namespace", "class", "struct", "union", "enum", "type alias", "function", "method", "constant")
## What a C/C++ type keyword declares.
CPP_TYPE_KINDS = {"class": "class", "struct": "struct", "union": "union", "enum": "enum"}

## UPPER_CASE (constants), with an optional leading underscore.
UPPER = re.compile(r"_?[A-Z][A-Z0-9_]*")
## CamelCase (types and type aliases), with an optional leading underscore.
CAMEL = re.compile(r"_?[A-Z][A-Za-z0-9]*")
## End of a sentence: ".", "!" or "?" before a space or the end, but not in "e.g." / "i.e." / "vs." / "...".
SENTENCE_END = re.compile(r"(?<!\be\.g)(?<!\bi\.e)(?<!\bvs)(?<!\bcf)(?<!\.\.)[.!?](?=\s|$)")
## A Doxygen command opening a comment line ("@param", "\\return", ...).
DOXYGEN_COMMAND = re.compile(r"^[@\\](\w+)\b\s*")
## A lone file name after `@file`.
FILE_NAME = re.compile(r"^[\w./-]+\.\w+$")

## `namespace a::b` (or an anonymous namespace), optionally inline.
CPP_NAMESPACE = re.compile(r"^(?:inline\s+)?namespace(?:\s+([\w:]+))?\s*(?:\{.*)?$")
## `extern "C"` (string contents are stripped before matching).
CPP_EXTERN = re.compile(r'^extern\s*""\s*(?:\{.*)?$')
## A type definition head: keyword, optional alignas / attributes, name, optional base list or underlying type.
CPP_TYPE = re.compile(r"^(?:typedef\s+)?(?P<keyword>class|struct|union|enum(?:\s+(?:class|struct))?)\b"
                      r"(?:\s+alignas\s*\([^)]*\))?(?:\s*\[\[[^\]]*\]\])?(?:\s+(?P<name>\w+))?\s*(?:final\b\s*)?"
                      r"(?::(?!:)[^;{]*)?(\{.*)?$")
## A leading `template <...>` (one nesting level of angle brackets).
CPP_TEMPLATE = re.compile(r"^template\s*<(?:[^<>]|<[^<>]*>)*>\s*")
## Leading attributes such as `[[nodiscard]]`.
CPP_ATTRIBUTES = re.compile(r"^(?:\[\[[^\]]*\]\]\s*)+")
## `using Name = ...`.
CPP_USING = re.compile(r"^using\s+(\w+)\s*=")
## `typedef ... Name;` (also function pointers: `typedef void (*Name)(...)`).
CPP_TYPEDEF = re.compile(r"^typedef\b(.*);")
## The name a function pointer typedef declares.
CPP_TYPEDEF_POINTER = re.compile(r"\(\s*[*&]\s*(\w+)\s*\)")
## The last identifier of a typedef, before any array bounds.
CPP_TYPEDEF_NAME = re.compile(r"(\w+)\s*(?:\[[^\]]*\]\s*)*$")
## A function's (possibly qualified) name or operator at the end of the text before its "(".
CPP_FUNCTION_NAME = re.compile(r"((?:\w+::)*(?:operator\s*(?:\(\)|[^\s(\w]+|\s\w[\w\s:*&<>]*)|~?\w+))\s*$")
## Words that end the text before a "(" but do not name a function.
CPP_KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "alignof", "alignas", "decltype",
                "static_assert", "catch", "defined", "new", "delete", "throw", "case", "do", "else", "noexcept",
                "requires", "co_return", "co_await", "co_yield", "__attribute__", "__declspec", "asm"}
## An operator name (its "=" is not an initializer).
CPP_OPERATOR = re.compile(r"\boperator\s*\S*")
## Words that may not start a declaration (statements that leak into declaration scope).
CPP_STATEMENT_STARTS = {"return", "else", "case", "goto", "break", "continue", "throw", "delete", "using"}
## Words that make a declaration a constant.
CPP_CONSTANT_WORDS = re.compile(r"\b(?:constexpr|constinit|const)\b")
## A declared variable's name: before its initializer, its array bounds, or its ";".
CPP_VARIABLE_NAME = re.compile(r"(\w+)\s*(?:\[[^\]]*\]\s*)*(?:=|\{|;|$)")
## `#define NAME value` (not a function-like macro, not an include guard without a value).
CPP_DEFINE = re.compile(r"^#\s*define\s+([A-Za-z_]\w*)(?!\()\s+\S")
## Preprocessor lines that may come before a file's header comment.
CPP_PRELUDE = re.compile(r"^#\s*(?:pragma\s+once|ifndef\s+\w+|define\s+\w+\s*)$")

## Scope kind: the file itself (scope kinds: declarations are read only in the file, namespace, extern and class
## scopes; SCOPE_BODY covers function bodies, enums and initializers).
SCOPE_FILE = "file"
## Scope kind: a namespace body.
SCOPE_NAMESPACE = "namespace"
## Scope kind: an extern "C" block.
SCOPE_EXTERN = "extern"
## Scope kind: a class, struct or union body.
SCOPE_CLASS = "class"
## Scope kind: anything else in braces (function bodies, enums, initializers): no declarations are read there.
SCOPE_BODY = "body"
## Scopes whose lines are declarations.
DECLARATION_SCOPES = {SCOPE_FILE, SCOPE_NAMESPACE, SCOPE_EXTERN, SCOPE_CLASS}


@dataclass(frozen=True)
class Symbol:
   """!One symbol a file defines."""

   ## Kind (one of KIND_ORDER).
   kind: str
   ## Name, qualified by its class where it has one (`Box.area`, `Box::area`).
   name: str
   ## Line (1-based) where it is declared.
   line: int


@dataclass
class SourceFile:
   """!One source file of the map."""

   ## Path relative to the repository root, with "/" separators.
   path: str
   ## One-line purpose (empty when the file has no header).
   purpose: str
   ## Symbols in source order.
   symbols: list[Symbol] = field(default_factory=list)


@dataclass(frozen=True)
class Match:
   """!One `find` result."""

   ## Path relative to the repository root.
   path: str
   ## Line (1-based).
   line: int
   ## Symbol kind.
   kind: str
   ## Symbol name.
   name: str

   def __str__(self) -> str:
      """!As `path:line kind name`.
      @return the text
      """
      return f"{self.path}:{self.line} {self.kind} {self.name}"


# -- purposes ----------------------------------------------------------------------------------------------------------


def first_sentence(text: str) -> str:
   """!The first sentence of a paragraph, on one line.
   @param text  the paragraph (any whitespace)
   @return the sentence (the whole paragraph when it has no sentence end)
   """
   flat = " ".join(text.split())
   end = SENTENCE_END.search(flat)
   return flat[:end.end()] if end else flat


def purpose_from_comment(lines: list[str]) -> str:
   """!A purpose from a header comment's text lines: the `@brief`, else the `@file` text, else the first paragraph.
   @param lines  comment text, one entry per line, markers removed ("" for an empty comment line)
   @return the first sentence of that paragraph, or ""
   """
   start = None
   for tag in ("brief", "file"):
      for i, line in enumerate(lines):
         command = DOXYGEN_COMMAND.match(line)
         if command and command.group(1) == tag:
            rest = line[command.end():].strip()
            if tag == "file" and FILE_NAME.match(rest):
               rest = ""   # "@file name.h": the name is not the purpose
            start = i
            lines = lines[:i] + [rest] + lines[i + 1:]
            break
      if start is not None:
         break
   if start is None:
      start = next((i for i, line in enumerate(lines) if line.strip()), len(lines))
   # Skip an empty first line (e.g. a bare "@file" line), then read up to an empty line or the next command.
   while start < len(lines) and not lines[start].strip():
      start += 1
   paragraph: list[str] = []
   for line in lines[start:]:
      if not line.strip() or (paragraph and DOXYGEN_COMMAND.match(line)):
         break
      paragraph.append(line)
   return first_sentence(" ".join(paragraph))


def cpp_purpose(text: str) -> str:
   """!A C/C++ file's purpose, from the comment block it starts with.
   @param text  the source
   @return the purpose, or ""
   """
   comment: list[str] = []
   in_block = False
   for raw in text.splitlines():
      stripped = raw.strip()
      if in_block:
         end = stripped.find("*/")
         part = stripped if end < 0 else stripped[:end]
         comment.append(part.lstrip("*").strip())
         if end >= 0:
            in_block = False
         continue
      if not stripped:
         if comment:
            break   # an empty line ends the header comment
         continue
      if stripped.startswith("//"):
         comment.append(stripped.lstrip("/!").strip())   # "//", "///" and "//!"
         continue
      if stripped.startswith("/*"):
         body = stripped[MARKER_LENGTH:].lstrip("*!")
         end = body.find("*/")
         comment.append((body if end < 0 else body[:end]).strip())
         in_block = end < 0
         continue
      if not comment and CPP_PRELUDE.match(stripped):
         continue   # "#pragma once" or an include guard before the header
      break
   return purpose_from_comment(comment) if comment else ""


def python_purpose(docstring: str | None) -> str:
   """!A Python module's purpose, from its docstring's first paragraph.
   @param docstring  the docstring (None when there is none)
   @return the purpose, or ""
   """
   if not docstring:
      return ""
   return purpose_from_comment([line.strip() for line in docstring.lstrip("!").splitlines()])


# -- Python ------------------------------------------------------------------------------------------------------------


def _is_type_alias(value: ast.expr | None) -> bool:
   """!Whether an assigned value looks like a type (`dict[str, Any]`, `int | None`, `Path`).
   @param value  the value
   @return True for type-like expressions
   """
   if isinstance(value, ast.Subscript):
      return True
   if isinstance(value, ast.BinOp) and isinstance(value.op, ast.BitOr):
      return _is_type_alias(value.left) and _is_type_alias(value.right)
   return isinstance(value, (ast.Name, ast.Attribute)) or (isinstance(value, ast.Constant) and value.value is None)


def _assigned_names(node: ast.stmt) -> tuple[list[ast.Name], ast.expr | None, bool]:
   """!The plain names an assignment stores.
   @param node  an Assign or AnnAssign
   @return (names, assigned value, annotated as TypeAlias)
   """
   if isinstance(node, ast.Assign):
      names = [n for t in node.targets for n in ast.walk(t) if isinstance(n, ast.Name)]
      return names, node.value, False
   if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
      annotation = node.annotation
      alias = (isinstance(annotation, ast.Name) and annotation.id == "TypeAlias") or \
         (isinstance(annotation, ast.Attribute) and annotation.attr == "TypeAlias")
      return [node.target], node.value, alias
   return [], None, False


def _python_body(body: list[ast.stmt], owner: str, out: list[Symbol]) -> None:
   """!Symbols of a module or class body.
   @param body   statements
   @param owner  "" at module level, else the qualified class name plus "."
   @param out    symbols found (appended)
   @return None
   """
   type_alias = getattr(ast, "TypeAlias", None)   # `type X = ...` (Python 3.12+)
   for node in body:
      if isinstance(node, ast.ClassDef):
         out.append(Symbol("class", owner + node.name, node.lineno))
         _python_body(node.body, f"{owner}{node.name}.", out)
      elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
         out.append(Symbol("method" if owner else "function", owner + node.name, node.lineno))
      elif isinstance(node, (ast.Assign, ast.AnnAssign)):
         names, value, alias = _assigned_names(node)
         for name in names:
            if alias or (CAMEL.fullmatch(name.id) and not UPPER.fullmatch(name.id) and _is_type_alias(value)):
               out.append(Symbol("type alias", owner + name.id, node.lineno))
            elif UPPER.fullmatch(name.id) and len(name.id.strip("_")) > 1:
               out.append(Symbol("constant", owner + name.id, node.lineno))
      elif type_alias is not None and isinstance(node, type_alias):
         out.append(Symbol("type alias", owner + node.name.id, node.lineno))
      elif not owner and isinstance(node, ast.If):
         _python_body(node.body + node.orelse, owner, out)   # e.g. `if TYPE_CHECKING:` or version checks
      elif not owner and isinstance(node, ast.Try):
         handlers = [s for h in node.handlers for s in h.body]
         _python_body(node.body + handlers + node.orelse + node.finalbody, owner, out)


def read_python(text: str) -> tuple[str, list[Symbol]]:
   """!A Python file's purpose and symbols: classes with their methods, functions, UPPER_CASE constants and
   CamelCase type aliases (nested functions and locals are skipped).
   @param text  the source
   @return (purpose, symbols); a file that does not parse has neither
   """
   try:
      tree = ast.parse(text)
   except (SyntaxError, ValueError):
      return "", []
   symbols: list[Symbol] = []
   _python_body(tree.body, "", symbols)
   return python_purpose(ast.get_docstring(tree)), symbols


# -- C / C++ -----------------------------------------------------------------------------------------------------------


def _is_digit_separator(line: str, i: int) -> bool:
   """!Whether the quote at line[i] is a C++14 digit separator (`1'000`) rather than a character literal.
   @param line  the line
   @param i     index of the "'"
   @return True inside a number
   """
   start = i
   while start > 0 and (line[start - 1].isalnum() or line[start - 1] in "'_"):
      start -= 1
   return start < i and line[start].isdigit() and i + 1 < len(line) and line[i + 1].isalnum()


def strip_cpp(text: str) -> list[str]:
   """!The code of each line, without comments and string / character contents. Lines that continue a
   preprocessor directive become empty, so each directive is one line.
   @param text  the source
   @return one stripped line per source line
   """
   out: list[str] = []
   in_comment = False
   continued_directive = False
   for raw in text.replace("\r\n", "\n").split("\n"):
      if continued_directive:
         continued_directive = raw.rstrip().endswith("\\")
         out.append("")
         continue
      code: list[str] = []
      i = 0
      while i < len(raw):
         if in_comment:
            end = raw.find("*/", i)
            if end < 0:
               break
            in_comment = False
            i = end + MARKER_LENGTH
            continue
         if raw.startswith("//", i):
            break
         if raw.startswith("/*", i):
            in_comment = True
            i += MARKER_LENGTH
            continue
         c = raw[i]
         if c == '"' or (c == "'" and not _is_digit_separator(raw, i)):
            j = i + 1
            while j < len(raw) and raw[j] != c:
               j += MARKER_LENGTH if raw[j] == "\\" else 1   # skip an escaped character
            code.append(c + c)
            i = j + 1
            continue
         code.append(c)
         i += 1
      line = "".join(code).strip()
      if line.startswith("#"):
         continued_directive = raw.rstrip().endswith("\\")
      out.append(line)
   return out


@dataclass
class _Scope:
   """!One open brace scope while reading C/C++."""

   ## SCOPE_FILE, SCOPE_NAMESPACE, SCOPE_EXTERN, SCOPE_CLASS or SCOPE_BODY.
   kind: str
   ## Qualified name (namespaces and classes), or "".
   name: str = ""


class _CppReader:
   """!Reads the symbols of one C/C++ file, line by line."""

   ## Open scopes; the first is the file.
   scopes: list[_Scope]
   ## Scope the next "{" opens (None: a body).
   pending: _Scope | None

   def __init__(self) -> None:
      """!Starts at file scope."""
      ## Open scopes; the first is the file.
      self.scopes = [_Scope(SCOPE_FILE)]
      ## Scope the next "{" opens (None: a body).
      self.pending = None
      ## Symbols found.
      self.symbols: list[Symbol] = []

   def namespace(self) -> str:
      """!The qualified name of the innermost enclosing namespace.
      @return the name, or ""
      """
      for scope in reversed(self.scopes):
         if scope.kind == SCOPE_NAMESPACE and scope.name:
            return scope.name
      return ""

   def owner(self) -> str:
      """!The qualified name of the class whose body is being read.
      @return the name, or "" outside a class
      """
      top = self.scopes[-1]
      return top.name if top.kind == SCOPE_CLASS else ""

   def add(self, kind: str, name: str, line: int) -> None:
      """!Records a symbol.
      @param kind  its kind
      @param name  its name
      @param line  its line
      @return None
      """
      self.symbols.append(Symbol(kind, name, line))

   def classify(self, text: str, line: int) -> None:
      """!Reads one declaration-scope statement (joined across lines until its parentheses balance).
      @param text  the statement's code
      @param line  its first line
      @return None
      """
      text = CPP_ATTRIBUTES.sub("", CPP_TEMPLATE.sub("", text)).strip()
      if not text or text.startswith(("{", "}")):
         return
      if text.startswith("#"):
         define = CPP_DEFINE.match(text)
         if define and UPPER.fullmatch(define.group(1)) and not self.owner():
            self.add("constant", define.group(1), line)
         return
      owner = self.owner()
      first_word = text.split(None, 1)[0]
      if first_word in CPP_STATEMENT_STARTS and not CPP_USING.match(text):
         return
      # Namespaces and extern "C" blocks.
      m = CPP_NAMESPACE.match(text)
      if m and not owner:
         name = m.group(1) or ""
         outer = self.namespace()
         qualified = f"{outer}::{name}" if outer and name else name
         if name:
            self.add("namespace", qualified, line)
         self.pending = _Scope(SCOPE_NAMESPACE, qualified or outer)
         return
      if CPP_EXTERN.match(text):
         self.pending = _Scope(SCOPE_EXTERN)
         return
      # Types (a ";" before any "{" makes it a forward declaration or a variable).
      m = CPP_TYPE.match(text)
      head_end = text.find("{")
      forward = ";" in (text if head_end < 0 else text[:head_end])
      if m and not forward:
         keyword = m.group("keyword").split()[0]
         name = m.group("name")
         qualified = f"{owner}::{name}" if owner and name else (name or "")
         if name:
            self.add(CPP_TYPE_KINDS[keyword], qualified, line)
         is_class = keyword != "enum" and bool(name)
         self.pending = _Scope(SCOPE_CLASS, qualified) if is_class else None
         return
      # Type aliases.
      m = CPP_USING.match(text)
      if m:
         self.add("type alias", f"{owner}::{m.group(1)}" if owner else m.group(1), line)
         return
      m = CPP_TYPEDEF.match(text)
      if m:
         declared = CPP_TYPEDEF_POINTER.search(m.group(1)) or CPP_TYPEDEF_NAME.search(m.group(1))
         if declared:
            self.add("type alias", f"{owner}::{declared.group(1)}" if owner else declared.group(1), line)
         return
      if first_word == "using":
         return   # using-declarations and using-directives
      self.pending = None
      paren = text.find("(")
      before = text if paren < 0 else text[:paren]
      if paren >= 0 and "=" not in CPP_OPERATOR.sub("", before):
         self.function(before, owner, line)
         return
      # Constants (namespace or class level; class members need static or constexpr).
      if CPP_CONSTANT_WORDS.search(before):
         if owner and not re.search(r"\b(?:static|constexpr|constinit)\b", before):
            return
         m = CPP_VARIABLE_NAME.search(before)
         if m and m.group(1) not in {"const", "constexpr", "constinit"}:
            self.add("constant", f"{owner}::{m.group(1)}" if owner else m.group(1), line)

   def function(self, before: str, owner: str, line: int) -> None:
      """!Records a function or method declared by the text before its "(".
      @param before  the text before the parameter list
      @param owner   the enclosing class, or ""
      @param line    its line
      @return None
      """
      m = CPP_FUNCTION_NAME.search(before)
      if not m:
         return
      name = " ".join(m.group(1).split())
      prefix = before[:m.start()].strip()
      last = name.rsplit("::", 1)[-1]
      if last in CPP_KEYWORDS or prefix.endswith((".", "->", ",", "(")):
         return
      qualifier = name.rsplit("::", 1)[0] if "::" in name else ""
      class_name = owner.rsplit("::", 1)[-1]
      # Constructors and destructors have no return type; anything else without one is a macro call.
      special = last.lstrip("~") in {class_name, qualifier.rsplit("::", 1)[-1]} if (owner or qualifier) else False
      if not re.search(r"\w", prefix) and not special:
         return
      if owner:
         self.add("method", f"{owner}::{name}", line)
      elif qualifier and CAMEL.fullmatch(qualifier.rsplit("::", 1)[-1]):
         self.add("method", name, line)   # out-of-line member definition: Type::name
      else:
         self.add("function", name, line)
      self.pending = None   # a "{" after it opens its body

   def braces(self, text: str) -> None:
      """!Opens and closes scopes for the braces of a statement or body line.
      @param text  the code
      @return None
      """
      # the characters as locals (Doxygen 1.12 reads the members assigned after a literal compared here as new ones)
      opens, closes, ends = "{", "}", ";"
      for c in text:
         if c == opens:
            self.scopes.append(self.pending or _Scope(SCOPE_BODY))
            self.pending = None
         elif c == closes:
            if len(self.scopes) > 1:
               self.scopes.pop()
         elif c == ends:
            self.pending = None

   def read(self, text: str) -> list[Symbol]:
      """!Reads a whole file.
      @param text  the source
      @return symbols in source order
      """
      head = ""
      head_line = 0
      parens = 0   # open "(" of the statement being joined
      for n, code in enumerate(strip_cpp(text), 1):
         if not code:
            continue
         if head or self.scopes[-1].kind in DECLARATION_SCOPES:
            if not head:
               head_line = n
            head = f"{head} {code}" if head else code
            parens += code.count("(") - code.count(")")
            if parens > 0 and not code.startswith("#") and n - head_line < MAX_STATEMENT_LINES:
               continue   # a parameter list continues on the next line
            statement = head
            head = ""
            parens = 0
            self.classify(statement, head_line)
            self.braces(statement)
         elif not code.startswith("#"):
            self.braces(code)
      return self.symbols


def read_cpp(text: str) -> tuple[str, list[Symbol]]:
   """!A C/C++ file's purpose and symbols: namespaces, types, type aliases, functions, methods and constants
   (function bodies, enum values, locals and data members are skipped).
   @param text  the source
   @return (purpose, symbols)
   """
   return cpp_purpose(text), _CppReader().read(text)


# -- files -------------------------------------------------------------------------------------------------------------


def language_of(path: Path) -> str | None:
   """!Which reader a file needs.
   @param path  the file
   @return "python", "cpp", or None when it is not source code
   """
   if path.suffix == PYTHON_SUFFIX:
      return "python"
   if path.suffix in CPP_SUFFIXES:
      return "cpp"
   if not path.suffix:
      try:
         with path.open("rb") as f:
            first = f.readline()
      except OSError:
         return None
      if first.startswith(b"#!") and b"python" in first:
         return "python"
   return None


def is_skipped_dir(path: Path, root: Path) -> bool:
   """!Whether a folder is left out of the map: a skipped name, a build tree, a hidden folder, or a nested
   repository (it has its own `.git`).
   @param path  the folder
   @param root  the repository root
   @return True to skip it and everything under it
   """
   name = path.name
   if name in SKIPPED_DIRS or name.startswith(".") or name.startswith(SKIPPED_DIR_PREFIXES):
      return True
   return path != root and (path / ".git").exists()


def source_files(root: Path) -> list[Path]:
   """!The source files under a repository root.
   @param root  the root
   @return files, sorted by folder, then name (so each folder's files stay together)
   """
   out: list[Path] = []
   for folder, dirs, files in os.walk(root):
      here = Path(folder)
      dirs[:] = sorted(d for d in dirs if not is_skipped_dir(here / d, root))
      for name in files:
         path = here / name
         if path.is_file() and language_of(path) is not None:
            out.append(path)
   return sorted(out, key=lambda p: (p.parent.relative_to(root).as_posix(), p.name))


def read_file(path: Path, root: Path) -> SourceFile:
   """!Reads one source file.
   @param path  the file
   @param root  the repository root
   @return its purpose and symbols
   """
   text = path.read_text(encoding="utf-8", errors="replace")
   reader = read_python if language_of(path) == "python" else read_cpp
   purpose, symbols = reader(text)
   return SourceFile(path.relative_to(root).as_posix(), purpose, symbols)


def scan(root: Path) -> list[SourceFile]:
   """!Reads every source file of a repository.
   @param root  the root
   @return files, sorted by folder, then name
   """
   return [read_file(p, root) for p in source_files(root)]


# -- the map -----------------------------------------------------------------------------------------------------------


def tool_path(root: Path) -> str:
   """!How the map's instructions name this tool: its path from the root when it is inside it.
   @param root  the repository root
   @return the path
   """
   here = Path(__file__).resolve()
   try:
      return here.relative_to(root.resolve()).as_posix()
   except ValueError:
      return here.name


def render(files: list[SourceFile], tool: str) -> str:
   """!The map's text.
   @param files  the files (sorted by folder, then name)
   @param tool   how the instructions name this tool
   @return Markdown, ending with a newline
   """
   missing = sum(1 for f in files if not f.purpose)
   out = [MAP_TITLE, "",
          f"Generated by `{tool} build`; do not edit by hand. `python3 {tool} check` fails when it is out of date.",
          "",
          "How to use it: find the file here, run `python3 " + tool + " find NAME` for the exact line "
          "(`path:line kind name`), then read only that range. The map has no line numbers, so it changes only "
          "when a file or symbol is added, removed or renamed.",
          "",
          f"{len(files)} source files; {missing} without a purpose line (no module docstring or file header)."]
   folder = None
   for f in files:
      parent, _, name = f.path.rpartition("/")
      if parent != folder:
         folder = parent
         out += ["", f"## `{parent}/`" if parent else "## Repository root", ""]
      out.append(f"- **`{name}`**" + (f": {f.purpose}" if f.purpose else ""))
      by_kind: dict[str, set[str]] = {}
      for s in f.symbols:
         by_kind.setdefault(s.kind, set()).add(s.name)
      for kind in KIND_ORDER:
         if kind in by_kind:
            names = sorted(by_kind[kind], key=lambda n: (n.lower(), n))
            out.append(f"   - {kind}: `{', '.join(names)}`")
   return "\n".join(out) + "\n"


def build_map(root: Path) -> str:
   """!The map a repository should have.
   @param root  the root
   @return its text
   """
   return render(scan(root), tool_path(root))


def write_map(root: Path) -> Path:
   """!Writes CODE_MAP.md at a repository root.
   @param root  the root
   @return the written file
   """
   path = root / MAP_NAME
   path.write_text(build_map(root), encoding="utf-8", newline="\n")
   return path


def map_difference(root: Path) -> list[str]:
   """!How CODE_MAP.md differs from the map the code gives.
   @param root  the repository root
   @return unified diff lines (empty when it is current)
   """
   path = root / MAP_NAME
   current = path.read_text(encoding="utf-8") if path.is_file() else ""
   wanted = build_map(root)
   if current == wanted:
      return []
   return list(difflib.unified_diff(current.splitlines(), wanted.splitlines(), f"{MAP_NAME} (committed)",
                                    f"{MAP_NAME} (from the code)", lineterm=""))


def find(root: Path, query: str, kind: str | None = None, exact: bool = False) -> list[Match]:
   """!Symbols matching a name, read live from the code.
   @param root   the repository root
   @param query  the name: a case-insensitive part of it, or with exact, the whole name or its last part
   @param kind   only this kind (e.g. "function", "type alias" or "type-alias")
   @param exact  match whole names, case-sensitive
   @return matches, sorted by path and line
   """
   wanted_kind = kind.replace("-", " ").replace("_", " ").lower() if kind else None
   out: list[Match] = []
   for f in scan(root):
      for s in f.symbols:
         if wanted_kind and s.kind != wanted_kind:
            continue
         last = re.split(r"::|\.", s.name)[-1]
         hit = query in (s.name, last) if exact else query.lower() in s.name.lower()
         if hit:
            out.append(Match(f.path, s.line, s.kind, s.name))
   return sorted(out, key=lambda m: (m.path, m.line, m.name))


# -- command line ------------------------------------------------------------------------------------------------------


def default_root() -> Path:
   """!The nearest folder above this script that holds `.git`, else the current folder.
   @return the root
   """
   here = Path(__file__).resolve().parent
   for folder in (here, *here.parents):
      if (folder / ".git").exists():
         return folder
   return Path.cwd()


def main(argv: list[str] | None = None) -> int:
   """!Command line.
   @param argv  arguments (default: sys.argv[1:])
   @return 0 on success; 1 when the map is out of date or nothing was found
   """
   common = argparse.ArgumentParser(add_help=False)
   common.add_argument("--root", type=Path, default=None, help="repository root (default: the one holding this tool)")
   parser = argparse.ArgumentParser(description="Builds, checks and searches CODE_MAP.md.")
   commands = parser.add_subparsers(dest="command", required=True)
   commands.add_parser("build", parents=[common], help=f"write {MAP_NAME} at the root")
   commands.add_parser("check", parents=[common], help=f"fail when {MAP_NAME} is out of date")
   finder = commands.add_parser("find", parents=[common], help="print `path:line kind name` for matching symbols")
   finder.add_argument("name", help="a part of the name (case-insensitive)")
   finder.add_argument("--kind", help="only this kind: " + ", ".join(k.replace(" ", "-") for k in KIND_ORDER))
   finder.add_argument("--exact", action="store_true", help="whole name or its last part, case-sensitive")
   finder.add_argument("--json", action="store_true", help="print the matches as JSON")
   args = parser.parse_args(argv)
   root = (args.root or default_root()).resolve()

   if args.command == "build":
      path = write_map(root)
      print(f"wrote {path.relative_to(root).as_posix()}")
      return 0
   if args.command == "check":
      difference = map_difference(root)
      if not difference:
         return 0
      for line in difference[:MAX_DIFF_LINES]:
         print(line)
      if len(difference) > MAX_DIFF_LINES:
         print(f"... {len(difference) - MAX_DIFF_LINES} more line(s)")
      print(f"{MAP_NAME} is out of date: run `python3 {tool_path(root)} build`", file=sys.stderr)
      return 1
   matches = find(root, args.name, args.kind, args.exact)
   if args.json:
      print(json.dumps([asdict(m) for m in matches], indent=JSON_INDENT))
   else:
      for m in matches:
         print(m)
   return 0 if matches else 1


if __name__ == "__main__":
   sys.exit(main())
