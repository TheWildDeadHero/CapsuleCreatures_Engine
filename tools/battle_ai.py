#!/usr/bin/env python3
"""!Migrates a battle's parties from AI flags to AI profiles (D-26 section 1, G1; tools/battlec/ai.py).

    battle_ai.py <battle.json>... [--check]

Each flag set the parties use becomes a profile of the rules the engine's former scorer ran for it, with its
former numbers (battlec.ai.flag_rules()), named after its flags (BASIC_NAME for none); a party's "ai" names its
profile. The profile of no flags is added as the default one ("default": true: a party of records, an unowned one
and a party without "ai" use it, as they ran without flags before), so the compiled decisions stay the same. A
battle that already has "ai_profiles" is left as it is. The file is written as the studio writes JSON (one-space
indents, UTF-8, a final newline); --check only reports what would change.

Exit status: 0 when every battle was migrated (or would be, or needed nothing), 1 when one was refused, 2 for a
command line that is not understood.
"""
from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from battle_split import dump, write_all  # noqa: E402
from battlec.ai import FLAG_RULES, flag_rules  # noqa: E402
from battlec.common import ManifestError  # noqa: E402

## Exit status: migrated (or nothing to do).
EXIT_OK = 0
## Exit status: a battle was refused.
EXIT_REFUSED = 1
## Name of the profile of no flags (the default one).
BASIC_NAME = "basic"
## Joins flag names into a profile name.
NAME_JOIN = "_"
## A parsed JSON object.
JsonDict = dict[str, Any]


def profile_name(flags: Sequence[str]) -> str:
   """!The profile name of a flag set.
   @param flags  the party's former flags
   @return its name (BASIC_NAME for none)
   """
   ordered = [f for f in FLAG_RULES if f in flags]
   return NAME_JOIN.join(ordered) if ordered else BASIC_NAME


def migrate_battle(battle: JsonDict) -> tuple[JsonDict, list[str]]:
   """!A battle.json with AI profiles instead of party flags.
   @param battle  the content manifest
   @return (the migrated manifest, notes of what changed; none: nothing to do)
   @throws ManifestError on an unknown flag
   """
   if "ai_profiles" in battle:
      return battle, []
   parties = battle.get("parties", [])
   profiles: dict[str, list[dict[str, str]]] = {BASIC_NAME: flag_rules([])}
   notes = []
   out_parties = []
   for i, p in enumerate(parties):
      q = dict(p)
      flags = p.get("ai", [])
      if not isinstance(flags, list):
         out_parties.append(q)
         continue
      try:
         rules = flag_rules(flags)
      except ManifestError as e:
         raise ManifestError(f"parties[{i}].ai: {e}") from None
      name = profile_name(flags)
      profiles[name] = rules
      if flags:
         q["ai"] = name
      else:
         q.pop("ai", None)   # the default profile
      notes.append(f"party {p.get('name', i)!r}: ai {flags} -> profile {name!r}")
      out_parties.append(q)
   out: JsonDict = {}
   for key, value in battle.items():
      if key == "parties":
         out["ai_profiles"] = [{"name": n, **({"default": True} if n == BASIC_NAME else {}), "rules": r}
                               for n, r in profiles.items()]
         out[key] = out_parties
      else:
         out[key] = value
   if "ai_profiles" not in out:
      out["ai_profiles"] = [{"name": BASIC_NAME, "default": True, "rules": profiles[BASIC_NAME]}]
   return out, notes + [f"ai_profiles: {', '.join(profiles)}"]


def migrate_file(path: str | Path, write: bool = True) -> list[str]:
   """!Migrates one battle.json on disk.
   @param path   the battle.json
   @param write  false: only report
   @return what changed (empty: nothing)
   @throws ManifestError when it cannot be read or is refused
   """
   path = Path(path)
   try:
      battle = json.loads(path.read_text(encoding="utf-8"))
   except OSError as e:
      raise ManifestError(f"{path}: cannot read ({e.strerror})") from None
   except json.JSONDecodeError as e:
      raise ManifestError(f"{path}: invalid JSON ({e})") from None
   out, notes = migrate_battle(battle)
   if notes and write:
      write_all([(path, dump(out))])
   return notes


def main(argv: Sequence[str]) -> int:
   """!Command line.
   @param argv  arguments
   @return exit status
   """
   ap = argparse.ArgumentParser(description="Turns party AI flags into AI profiles (D-26 section 1).")
   ap.add_argument("battle", nargs="+", help="battle.json files")
   ap.add_argument("--check", action="store_true", help="only report what would change")
   args = ap.parse_args(argv)
   status = EXIT_OK
   for b in args.battle:
      try:
         notes = migrate_file(b, write=not args.check)
      except ManifestError as e:
         print(f"battle_ai: {e}", file=sys.stderr)
         status = EXIT_REFUSED
         continue
      for n in notes or ["nothing to migrate"]:
         print(f"{b}: {n}")
   return status


if __name__ == "__main__":
   sys.exit(main(sys.argv[1:]))
