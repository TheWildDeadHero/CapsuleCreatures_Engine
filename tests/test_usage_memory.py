#!/usr/bin/env python3
"""!Tests for the GBA memory report (D-42 §4.6): owners from mangled names (tools/usage_memory.py), a small invented
symbol listing measured and checked against the regions and the budgets, `usage.py memory`, the memory budgets of
tools/build_settings.py and the link launcher (tools/gba_link.py) with a stand-in linker and nm."""
from __future__ import annotations

import io
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

## The engine's root.
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import build_settings  # noqa: E402
import gba_link  # noqa: E402
import usage  # noqa: E402
import usage_memory as um  # noqa: E402

## The invented program's IWRAM end (__iheap_start): 0x400 bytes of IWRAM code and data with padding.
IWRAM_USED = 0x400
## Its EWRAM end (__sbss_end__): 0x900 bytes.
EWRAM_USED = 0x900
## An nm listing (`nm -S --defined-only`): an engine mixer in IWRAM and its ring, a Thumb veneer to it, the engine's
## random numbers (capsule::rng counts as capsule::core), a framework state in EWRAM and in IWRAM, a game's deeper
## namespace, an LTO copy, the C library's data, ROM code and the linker's own symbols.
LISTING = f"""\
03000000 00000200 t _ZN7capsule5audio6detail12_GLOBAL__N_17mix_pcmERNS1_7ChannelEPmm
03000200 0000000c t ___ZN7capsule5audio6detail12_GLOBAL__N_17mix_pcmERNS1_7ChannelEPmm_from_thumb
03000210 00000100 b _ZN7capsule5audio6detail6g_ringE
03000310 00000040 b _ZN7capsule3rng12_GLOBAL__N_1L7g_stateE
03000350 00000020 b _ZN6pocket6battle2uiL6g_menuE
03000370 00000010 d _impure_data
02000000 00000800 b _ZN14capsule_import7orchard5story12_GLOBAL__N_1L6g_saveE
02000800 00000080 b _ZN14capsule_import7orchard4port12_GLOBAL__N_1L1gE.lto_priv.0
02000880 00000080 B _ZN6pocket6battle5state7g_stateE
08000000 00000100 T main
08000100 00000080 T _ZN7capsule6script3runEv
{0x3000000 + IWRAM_USED:08x} D __iheap_start
{0x2000000 + EWRAM_USED:08x} A __sbss_end__
03007f00 A __sp_usr
00000010 a __capsule_stack_reserve
"""
## The mixer's bytes and its veneer's.
MIXER = 0x200 + 0xC
## The engine audio module's IWRAM data.
AUDIO_DATA = 0x100
## The battle framework's EWRAM.
BATTLE_EWRAM = 0x80
## The imported game's story save.
SAVE = 0x800


def write_tool(folder: Path, name: str, body: str) -> Path:
   """!Writes a stand-in program (a Python script run directly).
   @param folder  where
   @param name    its file name
   @param body    its Python code
   @return its path
   """
   path = folder / name
   path.write_text(f"#!{sys.executable}\n" + body, encoding="utf-8")
   path.chmod(path.stat().st_mode | stat.S_IXUSR)
   return path


## A stand-in nm: prints the "program" (the stand-in linker writes the listing as the program's bytes).
FAKE_NM = """\
import sys
sys.stdout.write(open(sys.argv[-1], encoding="utf-8").read())
"""
## A stand-in linker: fails with the memory checks' assertion (or the NO_FIT_TEXT environment variable) when given
## gba_memory.ld; otherwise writes the listing (from the LISTING_FILE environment variable) as the program, and with
## --noinhibit-exec still fails, as a link past its region does. It records each command line in COMMANDS_FILE.
FAKE_LINKER = """\
import os, sys
args = sys.argv[1:]
with open(os.environ["COMMANDS_FILE"], "a", encoding="utf-8") as log:
   log.write(" ".join(args) + "\\n")
if any(a.endswith("gba_memory.ld") for a in args):
   default = "ld: GBA memory: IWRAM leaves less stack than CAPSULE_GBA_STACK_RESERVE"
   print(os.environ.get("NO_FIT_TEXT", default))
   sys.exit(1)
out = args[args.index("-o") + 1]
with open(out, "w", encoding="utf-8") as f:
   f.write(open(os.environ["LISTING_FILE"], encoding="utf-8").read())
sys.exit(1 if "-Wl,--noinhibit-exec" in args else 0)
"""


class Owners(unittest.TestCase):
   """Mangled names to scopes and owners."""

   def test_scopes(self) -> None:
      """Nested names, anonymous namespaces, internal linkage, local entities, special names, std, C names."""
      cases = {
         "_ZN7capsule5audio6g_ringE": ["capsule", "audio", "g_ring"],
         "_ZN14capsule_import7orchard5story12_GLOBAL__N_1L6g_saveE": ["capsule_import", "orchard", "story", "g_save"],
         "_ZZN7capsule5audio3mixEvE5cache": ["capsule", "audio", "mix"],
         "_ZGVZN6pocket6battle4drawEvE1x": ["pocket", "battle", "draw"],
         "_ZTVN6pocket6battle5SceneE": ["pocket", "battle", "Scene"],
         "_ZNK7capsule3rng3Rng4nextEv": ["capsule", "rng", "Rng", "next"],
         "_ZNSt6vectorIiE9push_backEv": ["std", "vector"],
         "_ZL5g_foo": ["g_foo"],
         "_ZN12_GLOBAL__N_15g_tmpE": ["g_tmp"],
         "memcpy": [],
      }
      for name, scopes in cases.items():
         self.assertEqual(um.namespace_path(name), scopes, name)

   def test_owners(self) -> None:
      """Two namespaces at most; the engine's other-module namespaces; the global namespace; veneers."""
      self.assertEqual(um.owner_of("_ZN14capsule_import7orchard5story12_GLOBAL__N_1L6g_saveE"),
                       "capsule_import::orchard")
      self.assertEqual(um.owner_of("_ZNK7capsule3rng3Rng4nextEv"), "capsule::core")
      self.assertEqual(um.owner_of("_ZN6pocket6g_rootE"), "pocket")
      self.assertEqual(um.owner_of("_ZL5g_foo"), um.GLOBAL_OWNER)
      self.assertEqual(um.owner_of("__sf"), um.GLOBAL_OWNER)
      self.assertEqual(um.target_of("___ZN7capsule5audio3mixEv_from_arm"), "_ZN7capsule5audio3mixEv")

   def test_shown_names(self) -> None:
      """Parameter lists and anonymous namespaces are left out; copies' suffixes too, without a demangler."""
      self.assertEqual(um.short_name("capsule::audio::(anonymous namespace)::mix(Channel&, unsigned long*) const"),
                       "capsule::audio::mix")
      self.assertEqual(um.short_name("f(void (*)(int))"), "f")
      shown = um.demangle(["_ZN1a1gE.lto_priv.0", "__x_from_arm"], None)
      self.assertEqual(shown, {"_ZN1a1gE.lto_priv.0": "_ZN1a1gE", "__x_from_arm": "x" + um.VENEER_TEXT})


class Report(unittest.TestCase):
   """The invented listing measured and checked."""

   def test_regions_and_owners(self) -> None:
      """The linker's ends give the regions' use; each owner's columns; the limits pass."""
      r = um.report(LISTING, game="invented")
      self.assertEqual(r["schema"], um.MEMORY_SCHEMA)
      self.assertEqual(r["regions"][um.IWRAM]["used"], IWRAM_USED)
      self.assertEqual(r["regions"][um.IWRAM]["limit"], um.IWRAM_BYTES - um.DEFAULT_STACK_RESERVE)
      self.assertEqual(r["regions"][um.EWRAM]["used"], EWRAM_USED)
      audio = r["owners"]["capsule::audio"]
      self.assertEqual((audio[um.IWRAM_CODE], audio[um.IWRAM_DATA]), (MIXER, AUDIO_DATA))
      self.assertEqual(r["owners"]["capsule::core"][um.IWRAM_DATA], 0x40)
      self.assertEqual(r["owners"]["capsule_import::orchard"][um.EWRAM], SAVE + 0x80)
      self.assertEqual(r["owners"]["pocket::battle"][um.EWRAM], BATTLE_EWRAM)
      self.assertEqual(r["owners"][um.GLOBAL_OWNER][um.ROM], 0x100)
      self.assertEqual(list(r["owners"])[0], "capsule::audio")   # the most IWRAM first
      self.assertEqual(r["over"], [])
      self.assertIn("GBA memory by owner (invented)", um.report_text(r))

   def test_without_linker_symbols(self) -> None:
      """A listing without the linker's ends sums its symbols."""
      text = "\n".join(line for line in LISTING.splitlines() if "__" not in line.split()[-1][:2])
      r = um.report(text)
      self.assertEqual(r["regions"][um.EWRAM]["used"], SAVE + 0x80 + BATTLE_EWRAM)

   def test_stack_reserve(self) -> None:
      """A reserve that leaves less than the program fails the IWRAM check, naming the largest owners."""
      reserve = um.IWRAM_BYTES - IWRAM_USED + 1
      r = um.report(LISTING, stack_reserve=reserve)
      self.assertEqual(len(r["over"]), 1)
      check = r["over"][0]
      self.assertEqual((check["owner"], check["region"], check["over"]), (None, um.IWRAM, 1))
      self.assertEqual(check["contributors"][0]["owner"], "capsule::audio")
      self.assertIn("Over budget: IWRAM (code + data)", um.report_text(r))

   def test_budgets(self) -> None:
      """Region budgets and owner budgets at any depth; a budget kept is no failure."""
      budgets = {"regions": {um.EWRAM: EWRAM_USED - 1},
                 "owners": {"capsule::audio": {um.IWRAM_CODE: MIXER - 1, um.IWRAM_DATA: AUDIO_DATA},
                            "capsule_import::orchard::story": {um.EWRAM: SAVE - 1},
                            "capsule::core": {um.IWRAM: 0x40}}}
      r = um.report(LISTING, budgets)
      over = {(c["owner"], c["region"]): c["over"] for c in r["over"]}
      self.assertEqual(over, {(None, um.EWRAM): 1, ("capsule::audio", um.IWRAM_CODE): 1,
                              ("capsule_import::orchard::story", um.EWRAM): 1})
      deep = next(c for c in r["over"] if c["owner"] == "capsule_import::orchard::story")
      self.assertEqual(deep["contributors"][0]["bytes"], SAVE)   # the port's state is not under the story


class Command(unittest.TestCase):
   """`usage.py memory`."""

   def test_check_and_out(self) -> None:
      """--check fails on a passed budget; --out writes the report."""
      with tempfile.TemporaryDirectory() as tmp:
         folder = Path(tmp)
         listing = folder / "game.nm"
         listing.write_text(LISTING, encoding="utf-8")
         project = folder / "project.json"
         project.write_text(json.dumps({"build": {"memory_budgets": {"owners": {"pocket": {"ewram": 1}}}}}),
                            encoding="utf-8")
         out = folder / "usage" / "game.memory.json"
         with redirect_stdout(io.StringIO()) as text:
            status = usage.main(["memory", str(listing), "--project", str(project), "--out", str(out), "--check"])
         self.assertEqual(status, 1)
         self.assertIn("Over budget: pocket EWRAM", text.getvalue())
         self.assertEqual(json.loads(out.read_text(encoding="utf-8"))["schema"], um.MEMORY_SCHEMA)
         with redirect_stdout(io.StringIO()):
            self.assertEqual(usage.main(["memory", str(listing), "--project", str(project)]), 0)


class Budgets(unittest.TestCase):
   """The "memory_budgets" build setting."""

   def test_parsed(self) -> None:
      """Checked by parse() but no compile definition; read by memory_budgets()."""
      build = {"screen_arena_bytes": 4096,
               "memory_budgets": {"iwram": "0x7800", "owners": {"capsule::audio": {"iwram_code": 16384}}}}
      self.assertEqual(build_settings.parse(build), {"screen_arena_bytes": 4096})
      self.assertEqual(build_settings.memory_budgets(build),
                       {"regions": {"iwram": 0x7800}, "owners": {"capsule::audio": {"iwram_code": 16384}}})
      self.assertEqual(build_settings.memory_budgets({}), {"regions": {}, "owners": {}})

   def test_refused(self) -> None:
      """Unknown regions, owners that are no namespace, values past the region and non-objects are refused."""
      bad = [{"memory_budgets": {"vram": 1}},
             {"memory_budgets": {"owners": {"not a namespace": {"ewram": 1}}}},
             {"memory_budgets": {"iwram": build_settings.IWRAM_BYTES + 1}},
             {"memory_budgets": {"owners": {"pocket": 1}}},
             {"memory_budgets": []}]
      for build in bad:
         with self.assertRaises(build_settings.SettingsError, msg=str(build)):
            build_settings.parse(build)

   def test_command_line(self) -> None:
      """--memory-budgets prints them; --cmake is unchanged by them."""
      with tempfile.TemporaryDirectory() as tmp:
         project = Path(tmp) / "project.json"
         project.write_text(json.dumps({"build": {"memory_budgets": {"ewram": 1024}}}), encoding="utf-8")
         tool = str(ROOT / "tools" / "build_settings.py")
         shown = subprocess.run([sys.executable, tool, str(project), "--memory-budgets"], capture_output=True,
                                text=True, check=True)
         self.assertEqual(json.loads(shown.stdout)["regions"], {"ewram": 1024})
         defs = subprocess.run([sys.executable, tool, str(project), "--cmake"], capture_output=True, text=True,
                               check=True)
         self.assertNotIn("memory", defs.stdout.lower())


@unittest.skipIf(os.name == "nt", "the stand-in tools run as scripts")
class Launcher(unittest.TestCase):
   """tools/gba_link.py with a stand-in linker and nm."""

   def setUp(self) -> None:
      """The stand-ins in a temporary folder."""
      self.tmp = tempfile.TemporaryDirectory()
      self.folder = Path(self.tmp.name)
      self.nm = write_tool(self.folder, "fake_nm", FAKE_NM)
      self.linker = write_tool(self.folder, "fake_ld", FAKE_LINKER)
      listing = self.folder / "listing.nm"
      listing.write_text(LISTING, encoding="utf-8")
      self.commands = self.folder / "commands.txt"
      self.env = {"LISTING_FILE": str(listing), "COMMANDS_FILE": str(self.commands)}
      self.saved = {k: os.environ.get(k) for k in self.env}
      os.environ.update(self.env)
      self.program = self.folder / "game.elf"
      self.script = self.folder / "gba_memory.ld"
      self.out = self.folder / "usage" / "game.memory.json"

   def tearDown(self) -> None:
      """Puts the environment back."""
      for k, v in self.saved.items():
         if v is None:
            os.environ.pop(k, None)
         else:
            os.environ[k] = v
      self.tmp.cleanup()

   def launch(self, *options: str, checks: bool = False) -> tuple[int, str]:
      """!Runs the launcher on the stand-in link.
      @param options  more launcher options
      @param checks   give the link the memory checks' script
      @return (status, printed text)
      """
      link = [str(self.linker), "main.o", f"-Wl,-Map,{self.folder / 'game.map'}", "-o", str(self.program)]
      if checks:
         link.append(str(self.script))
      args = ["--nm", str(self.nm), "--game", "game", "--out", str(self.out), "--memory-script", str(self.script),
              *options, "--", *link]
      with redirect_stdout(io.StringIO()) as text, redirect_stderr(io.StringIO()) as errors:
         status = gba_link.main(args)
      return status, text.getvalue() + errors.getvalue()

   def test_success(self) -> None:
      """A link that succeeds prints and writes its memory report."""
      status, text = self.launch()
      self.assertEqual(status, 0)
      self.assertTrue(self.program.exists())
      self.assertIn("GBA memory by owner (game)", text)
      self.assertEqual(json.loads(self.out.read_text(encoding="utf-8"))["game"], "game")

   def test_budget_fails(self) -> None:
      """With --check, a passed budget fails the build and removes the program."""
      project = self.folder / "project.json"
      project.write_text(json.dumps({"build": {"memory_budgets": {"owners": {"capsule::audio": {"iwram": 1}}}}}),
                         encoding="utf-8")
      status, text = self.launch("--project", str(project), "--check")
      self.assertEqual(status, 1)
      self.assertFalse(self.program.exists())
      self.assertIn("Over budget: capsule::audio IWRAM (code + data)", text)

   def test_does_not_fit(self) -> None:
      """A link that does not fit is measured by a second link beside it, without the checks, keeping its output
      and its own map; the first link's status is returned and the program is not written."""
      status, text = self.launch(checks=True)
      self.assertEqual(status, 1)
      self.assertFalse(self.program.exists())
      self.assertIn("GBA memory: IWRAM leaves less stack", text)
      self.assertIn("The program does not fit", text)
      self.assertIn("GBA memory by owner (game)", text)
      side = self.folder / f"game{gba_link.MEASURE_SUFFIX}.elf"
      self.assertTrue(side.exists())
      first, second = self.commands.read_text(encoding="utf-8").splitlines()
      self.assertIn(str(self.script), first)
      self.assertNotIn(str(self.script), second)
      self.assertIn(gba_link.KEEP_OUTPUT, second)
      self.assertIn(f"-Map,{side}.map", second)
      self.assertFalse(self.out.exists())   # a measurement is no build's report

   def test_section_past_region(self) -> None:
      """GNU ld's "is not within region" alone (an IWRAM section placed past the region, the assertions never
      reached) is a program that does not fit too."""
      os.environ["NO_FIT_TEXT"] = ("ld: address 0x300abd0 of game.elf section `.data' is not within region "
                                   "`iwram'")
      try:
         status, text = self.launch(checks=True)
      finally:
         os.environ.pop("NO_FIT_TEXT", None)
      self.assertEqual(status, 1)
      self.assertIn("The program does not fit", text)
      self.assertTrue((self.folder / f"game{gba_link.MEASURE_SUFFIX}.elf").exists())

   def test_other_failure(self) -> None:
      """A link that fails for another reason is passed through without a second link."""
      broken = write_tool(self.folder, "broken_ld", "import sys\nprint('undefined reference to foo')\nsys.exit(1)\n")
      with redirect_stdout(io.StringIO()) as text:
         status = gba_link.main(["--nm", str(self.nm), "--", str(broken), "-o", str(self.program)])
      self.assertEqual(status, 1)
      self.assertIn("undefined reference", text.getvalue())
      self.assertNotIn("does not fit", text.getvalue())


if __name__ == "__main__":
   unittest.main()
