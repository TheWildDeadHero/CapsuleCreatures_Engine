"""!The sample plugin's Studio extension (plugin API 3): a spreadsheet format for the opponents' moods and a preview.

The Studio imports this module (plugin.json "extension"); the engine's build never does, it runs connector.py only.
Like the connector it is pure Python: the format and the preview are plain text, so `capsule-studio ext export`,
`ext import` and `ext preview` run them without Qt, and the Plugin data panel shows the same.
"""
from __future__ import annotations

import csv
import io
from typing import Any

from capsule_studio.plugins.registry import FileFormat, Preview

## The connector's plugin id (its blocks are the ones exchanged and shown).
PLUGIN_ID = "sample.party_moods"
## The spreadsheet's columns.
COLUMNS = ("party", "mood", "bonus", "rival")


class MoodSheet(FileFormat):
   """!Every opponent's mood as a CSV row: party, mood, bonus, rival (an empty mood: no block)."""

   id = "moods-csv"
   title = "Opponent moods (CSV)"
   suffix = ".csv"
   plugin = PLUGIN_ID

   def export_data(self, data: Any) -> str:
      """!One row per party, with or without a block.
      @param data  core.plugin_data.PluginData
      @return the CSV text
      """
      out = io.StringIO()
      writer = csv.writer(out, lineterminator="\n")
      writer.writerow(COLUMNS)
      for target in data.targets("party"):
         block = data.get(PLUGIN_ID, "party", target.name)
         if block is None:
            writer.writerow([target.name, "", "", ""])
            continue
         full = data.complete(PLUGIN_ID, "party", block)
         writer.writerow([target.name, full["mood"], full["bonus"], full["rival"] or ""])
      return out.getvalue()

   def import_data(self, data: Any, text: str) -> list[tuple[str, str, Any]]:
      """!The rows as block changes.
      @param data  core.plugin_data.PluginData
      @param text  the CSV text
      @return [("party", name, block or None)]
      @throws ValueError when the header or a bonus is wrong
      """
      rows = list(csv.DictReader(io.StringIO(text)))
      if rows and tuple(rows[0].keys()) != COLUMNS:
         raise ValueError(f"the columns are {', '.join(COLUMNS)}")
      changes = []
      for row in rows:
         if not row["mood"]:
            changes.append(("party", row["party"], None))
            continue
         block: dict[str, Any] = {"mood": row["mood"]}
         if row["bonus"]:
            block["bonus"] = int(row["bonus"])
         if row["rival"]:
            block["rival"] = row["rival"]
         changes.append(("party", row["party"], block))
      return changes


class MoodPreview(Preview):
   """!A record's mood in a sentence."""

   id = "mood"
   title = "Mood"
   plugin = PLUGIN_ID
   attach_points = ("party", "map", "cell")

   def render(self, data: Any, attach: str, name: str, block: Any) -> str:
      """!The sentence.
      @param data    core.plugin_data.PluginData
      @param attach  "party", "map" or "cell"
      @param name    the record
      @param block   its block, defaults filled in
      @return text
      """
      if attach == "map":
         return f"{name} plays {block['music_mood']} music."
      if attach == "cell":
         return f"Standing at {name} feels {block['mood']}."
      lines = [f"{name} fights {block['mood']}."]
      if block["mood"] != "calm" and block["bonus"]:
         lines.append(f"The AI adds {block['bonus']} to its scores.")
      if block.get("rival"):
         lines.append(f"Rival: {block['rival']}.")
      return "\n".join(lines)


def register(ctx: Any) -> None:
   """!Adds the format and the preview.
   @param ctx  the Studio's ExtensionContext
   @return None
   """
   ctx.add_format(MoodSheet())
   ctx.add_preview(MoodPreview())
