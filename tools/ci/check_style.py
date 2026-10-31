#!/usr/bin/env python3
"""!Checks the project's code style (standard library only).

The rules (project instructions):

- every indent is 3 spaces per level, never a tab (all code: C/C++, Python,
  CMake, shell, PowerShell and batch files);
- C and C++ opening braces go on their own line;
- functions and members are snake_case; types and type aliases CamelCase;
- Python follows PEP-8 otherwise (lines of at most 120 characters), with type
  annotations on every function (each parameter but self / cls, and the
  return);
- no magic numbers: a number other than 0, 1 or -1 is a named constant
  (Python: an UPPER_CASE module or class constant; C/C++: constexpr /
  const / enum / preprocessor defines), except in test data (files under tests/).

Usage: check_style.py [--clang-format] PATH...  (files or folders). Prints
`file:line: rule: message` per problem and exits 1 when there is any.
--clang-format also runs `clang-format --dry-run --Werror` on C/C++ files
(the .clang-format beside the sources), when clang-format is installed.
"""
from __future__ import annotations

import argparse
import ast
import io
import re
import shutil
import subprocess
import sys
import tokenize
from dataclasses import dataclass
from pathlib import Path

## Spaces per indent level.
INDENT_WIDTH = 3
## Longest Python line (PEP-8, with the project's limit; setup.cfg says the same).
MAX_LINE_LENGTH = 120
## Numbers that never need a name.
ALLOWED_NUMBERS = {0, 1}
## Folders never checked.
SKIPPED_DIRS = {".git", ".github", ".venv", "__pycache__", "build", "build-docs", "ref_submodules", "submodules",
                "platform", "profiles", "wheels", "node_modules"}
## Folder names whose files hold test data (numbers there need no name).
TEST_DIRS = {"tests"}
## Length of a two-character comment marker ("//", "/*", "*/") or escape ("\\n").
MARKER_LENGTH = 2
## clang-format's diagnostic: file, line, column, message.
CLANG_FORMAT_DIAGNOSTIC = re.compile(r"(.+?):(\d+):\d+: (?:error|warning): (.*)")
## C/C++ source suffixes.
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc"}
## Script suffixes (and extension-less launchers starting with #!).
SCRIPT_SUFFIXES = {".sh", ".ps1", ".cmd", ".bat", ".cmake"}
## CMake files without the .cmake suffix.
CMAKE_NAMES = {"CMakeLists.txt"}

## snake_case (functions, members, locals), with leading underscores.
SNAKE = re.compile(r"_{0,2}[a-z][a-z0-9_]*_{0,2}")
## CamelCase (types and type aliases).
CAMEL = re.compile(r"_?[A-Z][A-Za-z0-9]*")
## UPPER_CASE (constants).
UPPER = re.compile(r"_?[A-Z][A-Z0-9_]*")
## Framework methods a subclass must override under their own (camelCase) names.
FRAMEWORK_OVERRIDES = re.compile(r"(setUp|tearDown|setUpClass|tearDownClass|setUpModule|tearDownModule|"
                                 r"eventFilter|sizeHint|minimumSizeHint|hasHeightForWidth|heightForWidth|"
                                 r"stepBy|textFromValue|valueFromText|"
                                 r"highlightBlock|initStyleOption|rowCount|columnCount|headerData|setData|"
                                 r"mimeData|mimeTypes|dropMimeData|supportedDropActions|flags|data|"
                                 r"[a-z]+(?:[A-Z][a-z]*)*Event|"
                                 r"paint|createEditor|setEditorData|setModelData|updateEditorGeometry|"
                                 r"visit_[A-Za-z_]+|find_spec)")

## A C/C++ numeric literal (decimal, hex, binary, octal; with suffixes).
CPP_NUMBER = re.compile(r"(?<![\w.])(0[xX][0-9A-Fa-f']+|0[bB][01']+|\d[\d']*\.?\d*(?:[eE][+-]?\d+)?)[uUlLfF]*(?![\w.])")
## Lines (after stripping) where C/C++ numbers are definitions, not magic.
CPP_DEFINITION = re.compile(r"^(#|static_assert\b|CAPSULE_POD\s*\(|.*\bconstexpr\b|.*\bconsteval\b|.*\bconstinit\b|"
                            r"(?:inline\s+)?(?:static\s+)?const\s+\w[\w:<>]*\s+k[A-Z]\w*\b.*=)")
## A C/C++ type declaration and its name.
CPP_TYPE = re.compile(r"^\s*(?:template\s*<[^>]*>\s*)?(?:struct|class|union|enum(?:\s+class)?)\s+(\w+)\s*(?:[:{]|$)")
## A C/C++ function definition or declaration at the start of a line, and its name.
CPP_FUNCTION = re.compile(r"^(?:[\w:<>,*&\s]+?)\s+[*&]*(\w+)\s*\([^;{]*\)\s*"
                          r"(?:const\s*)?(?:noexcept\s*)?(?:->[^;{]*)?$")
## C/C++ words that look like function names in CPP_FUNCTION but are not.
CPP_KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "alignof", "decltype", "static_assert",
                "catch", "defined", "operator", "main"}


@dataclass
class Problem:
   """!One style problem."""

   ## The file.
   path: Path
   ## Its line (1-based).
   line: int
   ## Rule id.
   rule: str
   ## What is wrong.
   message: str

   def __str__(self) -> str:
      """!As `file:line: rule: message`.
      @return the text
      """
      return f"{self.path}:{self.line}: {self.rule}: {self.message}"


def is_test_data(path: Path) -> bool:
   """!Whether a file holds test data (it is under a tests folder).
   @param path  the file
   @return True for test files
   """
   return any(part in TEST_DIRS for part in path.resolve().parts)   # resolved: "test_x.py" given from tests/ counts


# -- Python ------------------------------------------------------------------------------------------------------------


def check_python_indent(path: Path, text: str) -> list[Problem]:
   """!Block indents step by INDENT_WIDTH; no tabs.
   @param path  the file
   @param text  its source
   @return problems
   """
   out: list[Problem] = []
   levels = [0]
   for tok in tokenize.generate_tokens(io.StringIO(text).readline):
      if tok.type == tokenize.INDENT:
         if "\t" in tok.string:
            out.append(Problem(path, tok.start[0], "indent", "tab in an indent"))
         width = len(tok.string.expandtabs(INDENT_WIDTH))
         if width != levels[-1] + INDENT_WIDTH:
            out.append(Problem(path, tok.start[0], "indent",
                               f"block indented by {width - levels[-1]} spaces, not {INDENT_WIDTH}"))
         levels.append(width)
      elif tok.type == tokenize.DEDENT:
         levels.pop()
   for n, line in enumerate(text.splitlines(), 1):
      if "\t" in line[:len(line) - len(line.lstrip())]:
         out.append(Problem(path, n, "indent", "tab in an indent"))
      if len(line) > MAX_LINE_LENGTH:
         out.append(Problem(path, n, "line-length", f"{len(line)} characters, more than {MAX_LINE_LENGTH}"))
   return out


class _PythonVisitor(ast.NodeVisitor):
   """!Annotations, names and magic numbers of a Python module."""

   def __init__(self, path: Path) -> None:
      """!Starts a visit.
      @param path  the file
      """
      ## The file.
      self.path = path
      ## Problems found.
      self.problems: list[Problem] = []
      ## Scopes being visited: "module", "class" or "function".
      self.scopes: list[str] = ["module"]
      ## Whether numbers in the expression being visited are a named constant's value.
      self.in_constant = 0
      ## Whether numbers need no name in this file.
      self.test_data = is_test_data(path)

   def problem(self, node: ast.AST, rule: str, message: str) -> None:
      """!Records a problem.
      @param node     where
      @param rule     rule id
      @param message  what is wrong
      @return None
      """
      self.problems.append(Problem(self.path, getattr(node, "lineno", 0), rule, message))

   def _function(self, node: ast.FunctionDef | ast.AsyncFunctionDef) -> None:
      """!Checks a function's name and annotations, then visits its body.
      @param node  the function
      @return None
      """
      in_class = self.scopes[-1] == "class"
      if not (SNAKE.fullmatch(node.name) or (in_class and FRAMEWORK_OVERRIDES.fullmatch(node.name))):
         self.problem(node, "naming", f"function {node.name!r} is not snake_case")
      args = node.args
      params = args.posonlyargs + args.args + args.kwonlyargs
      decorators = {d.id for d in node.decorator_list if isinstance(d, ast.Name)}
      skip_first = in_class and "staticmethod" not in decorators and params
      for i, a in enumerate(params):
         if i == 0 and skip_first:
            continue
         if a.annotation is None:
            self.problem(a, "annotation", f"{node.name}(): parameter {a.arg!r} has no type annotation")
      for a in (args.vararg, args.kwarg):
         if a is not None and a.annotation is None:
            self.problem(a, "annotation", f"{node.name}(): parameter {a.arg!r} has no type annotation")
      if node.returns is None:
         self.problem(node, "annotation", f"{node.name}(): no return annotation")
      for d in node.decorator_list:
         self.visit(d)
      self._visit_defaults(args)
      self.scopes.append("function")
      for stmt in node.body:
         self.visit(stmt)
      self.scopes.pop()

   def _visit_defaults(self, args: ast.arguments) -> None:
      """!Visits parameter defaults and annotations (numbers in defaults are magic too).
      @param args  the arguments
      @return None
      """
      for d in args.defaults + [d for d in args.kw_defaults if d is not None]:
         self.visit(d)

   def visit_FunctionDef(self, node: ast.FunctionDef) -> None:
      """!A function.
      @param node  the node
      @return None
      """
      self._function(node)

   def visit_AsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:
      """!An async function.
      @param node  the node
      @return None
      """
      self._function(node)

   def visit_ClassDef(self, node: ast.ClassDef) -> None:
      """!A class: CamelCase.
      @param node  the node
      @return None
      """
      if not CAMEL.fullmatch(node.name):
         self.problem(node, "naming", f"class {node.name!r} is not CamelCase")
      for b in node.bases + node.keywords + node.decorator_list:
         self.visit(b)
      self.scopes.append("class")
      for stmt in node.body:
         self.visit(stmt)
      self.scopes.pop()

   def _constant_targets(self, targets: list[ast.expr]) -> bool:
      """!Whether an assignment at module or class level defines constants (UPPER_CASE, or CamelCase aliases).
      @param targets  assignment targets
      @return True when every name is a constant or type alias
      """
      if self.scopes[-1] == "function":
         return False
      names: list[str] = []
      for t in targets:
         for n in ast.walk(t):
            if isinstance(n, ast.Name):
               names.append(n.id)
            elif isinstance(n, (ast.Attribute, ast.Subscript)):
               return False
      return bool(names) and all(UPPER.fullmatch(n) or CAMEL.fullmatch(n) for n in names)

   def _check_store_names(self, targets: list[ast.expr]) -> None:
      """!Names an assignment stores: locals and members snake_case; module/class level may be constants.
      @param targets  assignment targets
      @return None
      """
      for t in targets:
         for n in ast.walk(t):
            if isinstance(n, ast.Attribute) and isinstance(n.ctx, ast.Store) \
                  and isinstance(n.value, ast.Name) and n.value.id == "self":
               if not SNAKE.fullmatch(n.attr):
                  self.problem(n, "naming", f"member {n.attr!r} is not snake_case")
            elif isinstance(n, ast.Name) and isinstance(n.ctx, ast.Store) and self.scopes[-1] == "function":
               if not (SNAKE.fullmatch(n.id) or UPPER.fullmatch(n.id) or n.id.strip("_") == ""):
                  self.problem(n, "naming", f"variable {n.id!r} is not snake_case")

   def visit_Assign(self, node: ast.Assign) -> None:
      """!An assignment.
      @param node  the node
      @return None
      """
      self._check_store_names(node.targets)
      constant = self._constant_targets(node.targets)
      self.in_constant += constant
      self.visit(node.value)
      self.in_constant -= constant

   def visit_AnnAssign(self, node: ast.AnnAssign) -> None:
      """!An annotated assignment.
      @param node  the node
      @return None
      """
      self._check_store_names([node.target])
      constant = self._constant_targets([node.target])
      self.in_constant += constant
      if node.value is not None:
         self.visit(node.value)
      self.in_constant -= constant

   def visit_Constant(self, node: ast.Constant) -> None:
      """!A literal: numbers other than 0 and 1 need a name.
      @param node  the node
      @return None
      """
      v = node.value
      if isinstance(v, bool) or not isinstance(v, (int, float, complex)):
         return
      if self.test_data or self.in_constant or v in ALLOWED_NUMBERS:
         return
      self.problem(node, "magic-number", f"{v!r}: give it a named constant")


def check_python(path: Path, text: str) -> list[Problem]:
   """!All Python rules for one file.
   @param path  the file
   @param text  its source
   @return problems
   """
   try:
      tree = ast.parse(text)
   except SyntaxError as e:
      return [Problem(path, e.lineno or 0, "syntax", str(e))]
   visitor = _PythonVisitor(path)
   visitor.visit(tree)
   return check_python_indent(path, text) + visitor.problems


# -- C / C++ -----------------------------------------------------------------------------------------------------------


def _strip_cpp(line: str, in_comment: bool) -> tuple[str, bool]:
   """!A line without comments and string / character literals.
   @param line        the line
   @param in_comment  a block comment is open at its start
   @return (code, block comment open at its end)
   """
   out = []
   i = 0
   while i < len(line):
      if in_comment:
         end = line.find("*/", i)
         if end < 0:
            return "".join(out), True
         i = end + MARKER_LENGTH
         in_comment = False
         continue
      c = line[i]
      if line.startswith("//", i):
         break
      if line.startswith("/*", i):
         in_comment = True
         i += MARKER_LENGTH
         continue
      if c in "\"'":
         j = i + 1
         while j < len(line) and line[j] != c:
            j += MARKER_LENGTH if line[j] == "\\" else 1   # skip an escaped character
         out.append(c + c)
         i = j + 1
         continue
      out.append(c)
      i += 1
   return "".join(out), in_comment


def check_cpp(path: Path, text: str) -> list[Problem]:
   """!C/C++ rules: tabs, braces on their own line, block indent, names, magic numbers.
   @param path  the file
   @param text  its source
   @return problems
   """
   out: list[Problem] = []
   lines = text.splitlines()
   codes: list[str] = []
   in_comment = False
   for line in lines:
      code, in_comment = _strip_cpp(line, in_comment)
      codes.append(code.rstrip())
   test_data = is_test_data(path)
   enum_depth = -1        # brace depth of the enum body being read, or -1
   in_definition = False  # inside a constant definition spanning lines (until its ";")
   depth = 0
   for n, (line, code) in enumerate(zip(lines, codes), 1):
      indent = len(line) - len(line.lstrip(" "))
      if "\t" in line[:len(line) - len(line.lstrip())]:
         out.append(Problem(path, n, "indent", "tab in an indent"))
      # A line comment on its own line sits on the indent grid too, unless it continues the trailing comment of the
      # line above (aligned under that comment, or under its text).
      if line.lstrip(" ").startswith("//") and indent % INDENT_WIDTH:
         above = lines[n - 1 - 1] if n > 1 else ""   # n is 1-based, lines 0-based: the line before
         marker = above.find("//")
         trailing = marker > 0 and above[:marker].strip() != ""   # code before the comment
         if not (trailing and indent >= marker):
            out.append(Problem(path, n, "indent", f"comment indented {indent} spaces, not a multiple of "
                                                  f"{INDENT_WIDTH}"))
      stripped = code.strip()
      if not stripped:
         continue
      # Braces: a block's "{" stands alone (initializer lists keep theirs).
      if stripped.endswith("{") and stripped != "{" and not stripped.startswith("#"):
         head = stripped[:-1].rstrip()
         # Braced initializers: "x = {", "return {", "f(a, {", "Type name{" (a declaration, not a block head).
         declaration = re.search(r"[\w>\]*&]\s+[*&]*\w+(\[\w*\])*$", head) is not None and not re.match(
            r"(struct|class|union|enum|namespace|extern|template)\b", head)
         initializer = head.endswith(("=", "return", ",", "(", "{")) or declaration
         if not initializer and not re.search(r"=\s*\{$", stripped):
            out.append(Problem(path, n, "braces", "opening brace on the same line (put it on the next line)"))
      # Block indent: the first line after a lone "{" is indented one level (namespaces, case and access labels: none).
      if stripped == "{":
         nxt = n
         while nxt < len(lines) and not codes[nxt].strip():
            nxt += 1
         prev = codes[n - 1 - 1].strip() if n > 1 else ""   # n is 1-based, codes 0-based: the line before
         if nxt < len(lines) and not codes[nxt].strip().startswith(("}", "#")):
            inner = len(lines[nxt]) - len(lines[nxt].lstrip(" "))
            same_level = prev.startswith(("namespace", "extern")) or \
               re.match(r"(case\b|default\s*:|public\s*:|private\s*:|protected\s*:)", codes[nxt].strip())
            want = indent if same_level else indent + INDENT_WIDTH
            if inner != want:
               out.append(Problem(path, nxt + 1, "indent", f"indented {inner - indent} after its brace, not "
                                                           f"{want - indent}"))
      # Names: types CamelCase, functions snake_case.
      m = CPP_TYPE.match(code)
      if m and not CAMEL.fullmatch(m.group(1)):
         out.append(Problem(path, n, "naming", f"type {m.group(1)!r} is not CamelCase"))
      if indent == 0 or depth == 0:
         m = CPP_FUNCTION.match(stripped)
         if m and m.group(1) not in CPP_KEYWORDS and not SNAKE.fullmatch(m.group(1)) \
               and not CAMEL.fullmatch(m.group(1)) and "operator" not in stripped:
            out.append(Problem(path, n, "naming", f"function {m.group(1)!r} is not snake_case"))
      # Enum bodies define their values.
      if re.match(r"(?:typedef\s+)?enum\b", stripped):
         enum_depth = depth
      # Magic numbers: outside definitions and test data.
      definition = in_definition or CPP_DEFINITION.match(stripped) is not None
      if definition and not stripped.startswith("#"):
         # A constant's value (after its "=") may continue on the next lines, up to its ";"; a constexpr
         # function's body does not count as a definition.
         in_definition = not stripped.endswith(";") and (in_definition or "=" in stripped)
      if not test_data and enum_depth < 0 and not definition:
         for m in CPP_NUMBER.finditer(stripped):
            literal = m.group(0)
            try:
               value = int(literal.rstrip("uUlL").replace("'", ""), 0)
            except ValueError:
               try:
                  value = float(literal.rstrip("fFlL").replace("'", ""))
               except ValueError:
                  continue
            if value not in ALLOWED_NUMBERS:
               out.append(Problem(path, n, "magic-number", f"{literal}: give it a named constant"))
      depth += stripped.count("{") - stripped.count("}")
      if enum_depth >= 0 and depth <= enum_depth and "}" in stripped:
         enum_depth = -1
   return out


def run_clang_format(paths: list[Path]) -> list[Problem]:
   """!clang-format's own check (the .clang-format next to the sources), when it is installed.
   @param paths  C/C++ files
   @return problems (none when clang-format is missing)
   """
   exe = shutil.which("clang-format")
   if exe is None or not paths:
      return []
   out: list[Problem] = []
   for p in paths:
      r = subprocess.run([exe, "--dry-run", "--Werror", str(p)], capture_output=True, text=True, check=False)
      for line in r.stderr.splitlines():
         m = CLANG_FORMAT_DIAGNOSTIC.match(line)
         if m:
            file, line_number, message = m.groups()
            out.append(Problem(Path(file), int(line_number), "clang-format", message))
   return out


# -- scripts -----------------------------------------------------------------------------------------------------------


## A heredoc start (shell): its terminator word.
HEREDOC = re.compile(r"<<-?\s*['\"]?([A-Za-z_]\w*)['\"]?")


def check_script(path: Path, text: str) -> list[Problem]:
   """!Shell, CMake, PowerShell and batch files: no tabs, step indents of INDENT_WIDTH (continuation lines and
   heredoc / here-string bodies are free).
   @param path  the file
   @param text  its source
   @return problems
   """
   out: list[Problem] = []
   cmake = path.suffix == ".cmake" or path.name in CMAKE_NAMES
   ps1 = path.suffix.lower() == ".ps1"
   terminator = None
   here_string = False
   parens = 0
   previous = ""
   for n, line in enumerate(text.replace("\r\n", "\n").splitlines(), 1):
      if terminator is not None:
         if line.strip() == terminator:
            terminator = None
         continue
      if here_string:
         if re.match(r"^[\"']@", line):
            here_string = False
         continue
      body = line.lstrip(" ")
      indent = len(line) - len(body)
      if line[:len(line) - len(line.lstrip())].count("\t"):
         out.append(Problem(path, n, "indent", "tab in an indent"))
      continuation = previous.rstrip().endswith(("\\", "`", ",", "|", "&&", "||", "(", "^")) or (cmake and parens > 0)
      if body and indent % INDENT_WIDTH and not continuation:
         out.append(Problem(path, n, "indent", f"indented {indent} spaces, not a multiple of {INDENT_WIDTH}"))
      if cmake:
         code = body.split("#", 1)[0]
         parens = max(parens + code.count("(") - code.count(")"), 0)
      elif ps1:
         if re.search(r"@[\"']\s*$", body):
            here_string = True
      else:
         m = HEREDOC.search(body)
         if m and not body.startswith("#"):
            terminator = m.group(1)
      if body.strip():
         previous = body
   return out


# -- driver ------------------------------------------------------------------------------------------------------------


def kind_of(path: Path) -> str | None:
   """!Which rules a file gets.
   @param path  the file
   @return "python", "cpp", "script", or None to skip it
   """
   if path.suffix == ".py":
      return "python"
   if path.suffix in CPP_SUFFIXES:
      return "cpp"
   if path.suffix.lower() in SCRIPT_SUFFIXES or path.name in CMAKE_NAMES:
      return "script"
   if not path.suffix:
      try:
         with path.open("rb") as f:
            first = f.readline()
      except OSError:
         return None
      if first.startswith(b"#!") and b"python" in first:
         return "python"
      if first.startswith(b"#!") and (b"sh" in first):
         return "script"
   return None


def files_in(paths: list[Path]) -> list[Path]:
   """!The files to check under the given files and folders.
   @param paths  files or folders
   @return files, sorted
   """
   out: set[Path] = set()
   for p in paths:
      if p.is_file():
         out.add(p)
         continue
      for f in p.rglob("*"):
         if f.is_file() and not any(part in SKIPPED_DIRS for part in f.relative_to(p).parts):
            out.add(f)
   return sorted(f for f in out if kind_of(f) is not None)


def check(paths: list[Path], clang_format: bool = False) -> list[Problem]:
   """!Checks files and folders.
   @param paths         files or folders
   @param clang_format  also run clang-format's check on C/C++ files
   @return problems
   """
   out: list[Problem] = []
   cpp_files: list[Path] = []
   for f in files_in(paths):
      text = f.read_text(encoding="utf-8")
      kind = kind_of(f)
      if kind == "python":
         out += check_python(f, text)
      elif kind == "cpp":
         out += check_cpp(f, text)
         cpp_files.append(f)
      else:
         out += check_script(f, text)
   if clang_format:
      out += run_clang_format(cpp_files)
   return out


def main(argv: list[str] | None = None) -> int:
   """!Command line.
   @param argv  arguments (default: sys.argv[1:])
   @return 0 when the code follows the style, 1 otherwise
   """
   p = argparse.ArgumentParser(description="Checks the project's code style.")
   p.add_argument("paths", nargs="+", type=Path, help="files or folders")
   p.add_argument("--clang-format", action="store_true", help="also run clang-format --dry-run on C/C++ files")
   p.add_argument("--rule", action="append", default=[], help="report only this rule (repeatable)")
   p.add_argument("--summary", action="store_true", help="print counts per rule instead of each problem")
   args = p.parse_args(argv)
   problems = check(args.paths, args.clang_format)
   if args.rule:
      problems = [x for x in problems if x.rule in args.rule]
   if args.summary:
      counts: dict[str, int] = {}
      for x in problems:
         counts[x.rule] = counts.get(x.rule, 0) + 1
      for rule, count in sorted(counts.items()):
         print(f"{rule}: {count}")
   else:
      for x in problems:
         print(x)
   if problems:
      print(f"{len(problems)} style problem(s)", file=sys.stderr)
      return 1
   return 0


if __name__ == "__main__":
   sys.exit(main())
