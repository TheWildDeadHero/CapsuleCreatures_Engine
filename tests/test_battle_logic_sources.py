#!/usr/bin/env python3
"""!Tests for the battle compiler's `.ceffect` source loading (tools/battlec/logic.py load_sources): editors pass
their unsaved effect sources the way they pass unsaved manifests (run directly or by unittest discovery)."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import battle_compiler as bc  # noqa: E402
from battlec.common import ManifestError  # noqa: E402
from battlec.logic import load_sources  # noqa: E402

## The test battle data.
FIXTURE = ROOT / "samples/starter/battle.json"
## Its condition logic (statuses and weathers).
CONDITIONS = (FIXTURE.parent / "effects/conditions.ceffect").resolve()


class UnsavedSourceTests(unittest.TestCase):
   """!Unsaved `.ceffect` text replaces the file it names; the file is still a dependency."""

   def test_unsaved_text_is_compiled(self) -> None:
      """!An unsaved source's text is what compiles (here: Scorch's point handler is gone), and an unsaved source
      with a syntax error fails the compile with its file and position.
      @return None
      """
      saved = bc.compile_battle(FIXTURE)
      text = CONDITIONS.read_text(encoding="utf-8")
      self.assertIn('status "Scorch"', text)
      unsaved = text.replace("point damage when", "point damage when 0 and", 1)
      deps: list[Path] = []
      edited = bc.compile_battle(FIXTURE, deps, {CONDITIONS: unsaved})
      self.assertIn(CONDITIONS, deps)
      self.assertEqual(len(edited["point_handlers"]), len(saved["point_handlers"]))
      self.assertNotEqual([h["ir"] for h in edited["point_handlers"]], [h["ir"] for h in saved["point_handlers"]])
      with self.assertRaisesRegex(ManifestError, r"effects/conditions\.ceffect:1:"):
         bc.compile_battle(FIXTURE, None, {CONDITIONS: "status {"})

   def test_files_without_unsaved_text(self) -> None:
      """!Without unsaved text the files are read; unsaved data that is not text is refused.
      @return None
      """
      paths = ["effects/conditions.ceffect"]
      from_file = load_sources(paths, FIXTURE.parent, "battle.json", None)
      self.assertEqual(load_sources(paths, FIXTURE.parent, "battle.json", None, {}), from_file)
      self.assertEqual(load_sources(paths, FIXTURE.parent, "battle.json", None, {CONDITIONS: ""}), [])
      with self.assertRaisesRegex(ManifestError, "must be its text"):
         load_sources(paths, FIXTURE.parent, "battle.json", None, {CONDITIONS: {"text": ""}})


if __name__ == "__main__":
   unittest.main()
