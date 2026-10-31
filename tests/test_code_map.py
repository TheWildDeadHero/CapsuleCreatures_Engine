#!/usr/bin/env python3
"""!Tests for tools/ci/code_map.py: symbols and purposes it reads, folders it skips, map stability, and find."""
from __future__ import annotations

import io
import json
import sys
import tempfile
import textwrap
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "ci"))
import code_map  # noqa: E402

## A header in the project's style.
HEADER = '''\
/// @file
/// Boxes and their areas (test header).
///
/// More detail that is not the purpose.
#pragma once

#include <cstdint>

namespace capsule::shapes
{

/// Sides of a box.
inline constexpr int kSides = 4;
#define SHAPES_LIMIT 8

/// Units.
using Length = uint16_t;
typedef void (*Visitor)(int value);

/// Box kinds.
enum class Kind : uint8_t
{
   Square,   ///< Four equal sides.
   Oblong    ///< Two pairs.
};

/// A box.
struct Box
{
   Length width;   ///< Width.
   static constexpr int kCorners = 4;

   /// Makes a box.
   Box(Length w);
   /// Area.
   /// @return width squared
   int area() const;
   bool operator==(const Box& other) const;
};

/// Draws a box.
/// @param box    the box
/// @param scale  its scale
void draw(const Box& box,
          int scale);

} // namespace capsule::shapes
'''

## A source file with bodies, comments and strings that must not show up.
SOURCE = '''\
/// @file
/// Box drawing.
#include "shapes.h"

namespace capsule::shapes
{

namespace
{
/// Local helper.
int helper(int x)
{
   struct Local
   {
      int y;
   };
   int value = helper(x - 1);   // void not_a_function();
   const char* text = "void fake();";
   auto f = [](int a) { return a; };
   return value;
}
} // namespace

/* int hidden(); */
Box::Box(Length w)
   : width(w)
{
}

int Box::area() const
{
   if (width)
   {
      return width * width;
   }
   return 0;
}

void draw(const Box& box, int scale)
{
   CAPSULE_ASSERT(scale > 0);
}

} // namespace capsule::shapes
'''

## A Python module.
PYTHON = '''\
"""!Packs shapes into atlases. Second sentence.

More detail.
"""
from __future__ import annotations

from typing import Any

## Pixels per tile.
TILE_SIZE = 8
## A JSON object.
JsonDict = dict[str, Any]


class Packer:
   """!Packs."""

   ## Most items.
   LIMIT = 4

   def pack(self, items: list[int]) -> int:
      """!Packs items."""
      def inner() -> int:
         return 0
      local = inner()
      return local


def load(path: str) -> JsonDict:
   """!Loads."""
   return {}
'''


def names(symbols: list[code_map.Symbol]) -> set[tuple[str, str]]:
   """!(kind, name) of each symbol.
   @param symbols  symbols
   @return the pairs
   """
   return {(s.kind, s.name) for s in symbols}


def make_repo(folder: Path) -> Path:
   """!A small repository: a header, a source and a Python tool.
   @param folder  where
   @return its root
   """
   (folder / ".git").mkdir()
   (folder / "include").mkdir()
   (folder / "src").mkdir()
   (folder / "tools").mkdir()
   (folder / "include" / "shapes.h").write_text(HEADER, encoding="utf-8")
   (folder / "src" / "shapes.cpp").write_text(SOURCE, encoding="utf-8")
   (folder / "tools" / "packer.py").write_text(PYTHON, encoding="utf-8")
   return folder


class CodeMapTests(unittest.TestCase):
   """!The code map tool."""

   def test_cpp_symbols(self) -> None:
      """!Header and source symbols, with kinds, qualified names, lines and the purpose."""
      purpose, symbols = code_map.read_cpp(HEADER)
      self.assertEqual(purpose, "Boxes and their areas (test header).")
      self.assertEqual(names(symbols), {
         ("namespace", "capsule::shapes"), ("constant", "kSides"), ("constant", "SHAPES_LIMIT"),
         ("type alias", "Length"), ("type alias", "Visitor"), ("enum", "Kind"), ("struct", "Box"),
         ("constant", "Box::kCorners"), ("method", "Box::Box"), ("method", "Box::area"),
         ("method", "Box::operator=="), ("function", "draw")})
      draw = next(s for s in symbols if s.name == "draw")
      self.assertEqual(HEADER.splitlines()[draw.line - 1].strip(), "void draw(const Box& box,")

      purpose, symbols = code_map.read_cpp(SOURCE)
      self.assertEqual(purpose, "Box drawing.")
      self.assertEqual(names(symbols), {
         ("namespace", "capsule::shapes"), ("function", "helper"), ("method", "Box::Box"),
         ("method", "Box::area"), ("function", "draw")})

   def test_bodies_and_comments_skipped(self) -> None:
      """!Locals, local types, lambdas, enum values, comments and strings are not symbols."""
      found = {s.name for s in code_map.read_cpp(SOURCE)[1]} | {s.name for s in code_map.read_cpp(HEADER)[1]}
      for name in ("Local", "value", "text", "f", "not_a_function", "fake", "hidden", "CAPSULE_ASSERT", "Square",
                   "width", "y"):
         self.assertNotIn(name, found)

   def test_python_symbols(self) -> None:
      """!Classes with methods, functions, constants and type aliases; nested functions and locals skipped."""
      purpose, symbols = code_map.read_python(PYTHON)
      self.assertEqual(purpose, "Packs shapes into atlases.")
      self.assertEqual(names(symbols), {
         ("constant", "TILE_SIZE"), ("type alias", "JsonDict"), ("class", "Packer"), ("constant", "Packer.LIMIT"),
         ("method", "Packer.pack"), ("function", "load")})
      self.assertEqual(code_map.read_python("x = 1\n")[0], "")   # no docstring: no purpose

   def test_skipped_folders(self) -> None:
      """!Builds, environments, generated, reference and third-party code and nested repositories are left out."""
      with tempfile.TemporaryDirectory(prefix="capsule_code_map_") as folder:
         root = make_repo(Path(folder))
         for skipped in ("build", "build-gba", ".venv/lib", "generated", "ref_submodules/example", "submodules/lib",
                         "third_party", "nested"):
            (root / skipped).mkdir(parents=True)
            (root / skipped / "ghost.py").write_text('"""!Ghost."""\n', encoding="utf-8")
         (root / "nested" / ".git").mkdir()
         paths = [f.path for f in code_map.scan(root)]
      self.assertEqual(paths, ["include/shapes.h", "src/shapes.cpp", "tools/packer.py"])

   def test_map_stability(self) -> None:
      """!A body edit keeps the map current; a new function makes it stale; the map has no line numbers."""
      with tempfile.TemporaryDirectory(prefix="capsule_code_map_") as folder:
         root = make_repo(Path(folder))
         code_map.write_map(root)
         text = (root / code_map.MAP_NAME).read_text(encoding="utf-8")
         self.assertIn("## `include/`", text)
         self.assertIn("- **`shapes.h`**: Boxes and their areas (test header).", text)
         self.assertIn("   - method: `Box::area, Box::Box, Box::operator==`", text)
         self.assertNotIn("shapes.h:", text)
         self.assertEqual(code_map.map_difference(root), [])

         source = root / "src" / "shapes.cpp"
         source.write_text(SOURCE.replace("return width * width;", "return width * width * 1;\n      // edited"),
                           encoding="utf-8")
         self.assertEqual(code_map.map_difference(root), [])

         source.write_text(SOURCE.replace("void draw(", "void erase(const Box& box)\n{\n}\n\nvoid draw("),
                           encoding="utf-8")
         self.assertNotEqual(code_map.map_difference(root), [])
         with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(code_map.main(["check", "--root", str(root)]), 1)

   def test_find(self) -> None:
      """!find: case-insensitive parts, exact names, kinds, JSON, and the exit status."""
      with tempfile.TemporaryDirectory(prefix="capsule_code_map_") as folder:
         root = make_repo(Path(folder))
         self.assertEqual([str(m) for m in code_map.find(root, "AREA")],
                          ["include/shapes.h:37 method Box::area", "src/shapes.cpp:30 method Box::area"])
         self.assertEqual([m.path for m in code_map.find(root, "Box", kind="struct")], ["include/shapes.h"])
         self.assertEqual([m.name for m in code_map.find(root, "pack", exact=True)], ["Packer.pack"])
         self.assertEqual([m.name for m in code_map.find(root, "Json", kind="type-alias")], ["JsonDict"])
         out = io.StringIO()
         with redirect_stdout(out):
            status = code_map.main(["find", "load", "--json", "--root", str(root)])
         self.assertEqual(status, 0)
         self.assertEqual(json.loads(out.getvalue()),
                          [{"path": "tools/packer.py", "line": 29, "kind": "function", "name": "load"}])
         with redirect_stdout(io.StringIO()):
            self.assertEqual(code_map.main(["find", "no_such_symbol", "--root", str(root)]), 1)


if __name__ == "__main__":
   unittest.main()
