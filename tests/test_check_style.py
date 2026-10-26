#!/usr/bin/env python3
"""!Tests for tools/ci/check_style.py: each rule finds what it should and nothing else."""
from __future__ import annotations

import sys
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "ci"))
import check_style as cs  # noqa: E402


def rules(problems: list[cs.Problem]) -> list[str]:
   """!The rule ids of problems, in order.
   @param problems  problems
   @return rule ids
   """
   return [p.rule for p in problems]


class PythonTests(unittest.TestCase):
   """!Python rules."""

   def py(self, text: str, path: str = "tools/x.py") -> list[str]:
      """!Checks a Python snippet.
      @param text  source (dedented)
      @param path  the file it pretends to be
      @return rule ids found
      """
      return rules(cs.check_python(Path(path), textwrap.dedent(text)))

   def test_clean(self) -> None:
      """!Annotated, 3-space, named constants: nothing to report."""
      self.assertEqual(self.py('''
         SIZE = 16


         class Box:
            """!A box."""

            def area(self, scale: int = 1) -> int:
               """!Area."""
               return SIZE * SIZE * scale
         '''), [])

   def test_indent(self) -> None:
      """!A 4-space block is reported."""
      self.assertEqual(self.py("def f() -> None:\n    pass\n"), ["indent"])

   def test_annotations(self) -> None:
      """!Each parameter (not self) and the return need annotations."""
      found = self.py('''
         class A:
            def m(self, x, y: int):
               pass
         ''')
      self.assertEqual(found, ["annotation", "annotation"])   # x, and the return

   def test_names(self) -> None:
      """!camelCase functions and members are reported; framework overrides are not."""
      found = self.py('''
         class A:
            def doIt(self) -> None:
               self.someValue = 1

            def setUp(self) -> None:
               _ = 1
         ''')
      self.assertEqual(found, ["naming", "naming"])

   def test_magic_numbers(self) -> None:
      """!Numbers other than 0, 1 and -1 need a constant, except in constants and test data."""
      text = '''
         LIMIT = 30
         def f(x: int) -> int:
            return x * 7 + LIMIT - 1
         '''
      self.assertEqual(self.py(text), ["magic-number"])
      self.assertEqual(self.py(text, "tests/test_x.py"), [])


class CppTests(unittest.TestCase):
   """!C/C++ rules."""

   def cpp(self, text: str, path: str = "src/x.cpp") -> list[str]:
      """!Checks a C++ snippet.
      @param text  source (dedented)
      @param path  the file it pretends to be
      @return rule ids found
      """
      return rules(cs.check_cpp(Path(path), textwrap.dedent(text)))

   def test_clean(self) -> None:
      """!Allman braces, 3 spaces, named constants, initializers on one line."""
      self.assertEqual(self.cpp('''
         inline constexpr int kSize = 16;
         const int kTable[] = {
            2, 3, 5,
         };

         struct Box
         {
            int area() const
            {
               const Box b{};
               return kSize * kSize;
            }
         };
         '''), [])

   def test_braces_and_indent(self) -> None:
      """!A same-line brace and a 4-space block are reported."""
      self.assertEqual(self.cpp('''
         void f() {
         }

         void g()
         {
             h();
         }
         '''), ["braces", "indent"])

   def test_comment_indent(self) -> None:
      """!A comment line off the 3-space grid is reported; one continuing a trailing comment is not."""
      self.assertEqual(self.cpp('''
         void f()
         {
             // one space too far
            int x = 0;   // a trailing comment
                         // that goes on here
         }
         '''), ["indent"])

   def test_names(self) -> None:
      """!A type that is not CamelCase and a function that is not snake_case are reported."""
      self.assertEqual(self.cpp('''
         struct my_box
         {
         };

         void doThing(int x)
         {
         }
         '''), ["naming", "naming"])

   def test_magic_numbers(self) -> None:
      """!Numbers outside definitions, enums and test data; comments and strings do not count."""
      text = '''
         enum class E
         {
            A = 4,
         };
         int f(int x)
         {
            return x * 12;   // 12 per row
         }
         const char* s = "42";
         '''
      self.assertEqual(self.cpp(text), ["magic-number"])
      self.assertEqual(self.cpp(text, "tests/x.cpp"), [])


class ScriptTests(unittest.TestCase):
   """!Shell, CMake and batch rules."""

   def test_steps_and_heredocs(self) -> None:
      """!Step indents of 3; continuations and heredoc bodies are free."""
      text = "if true; then\n   echo a \\\n        b\n    echo c\nfi\ncat <<EOF\n     free\nEOF\n"
      self.assertEqual(rules(cs.check_script(Path("x.sh"), text)), ["indent"])

   def test_cmake_parens(self) -> None:
      """!Lines inside an open command are continuations."""
      text = "if(A)\n   add_test(NAME x\n            COMMAND y)\nendif()\n"
      self.assertEqual(rules(cs.check_script(Path("CMakeLists.txt"), text)), [])


if __name__ == "__main__":
   unittest.main()
