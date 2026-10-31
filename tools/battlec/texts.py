"""!Texts of the battle compiler: the engine messages (capsule::battle::Msg)
with their placeholders, and the terms section ({@concept} words expanded at
compile time).
"""
from __future__ import annotations

import re
from collections.abc import Mapping

import text_encoder

from .common import PAIR, Json, ManifestError, Texts

## Engine message keys at their message ids (capsule::battle::Msg names the ones the engine says itself, with these
## values; the optional ones keep their ids, C4: the classic-shaped keys stay for the manifests that give them).
MESSAGES = [
   "appeared", "party_challenge", "send_out", "party_sends", "come_back", "used_move", "missed", "no_effect",
   "super_effective", "not_very_effective", "critical", "hit_count", "fainted", "stat_up", "stat_up2",
   "stat_down", "stat_down2", "stat_max", "stat_min", "confused", "confusion_hit", "confusion_end",
   "became_confused", "already_confused", "flinched", "protected", "protect_start", "recoil", "drained",
   "healed", "hp_full", "failed", "no_target", "passive", "item_used", "absorbed", "exp_gained", "level_up",
   "learned", "not_learned", "got_away", "cant_escape", "escape_blocked", "party_defeated", "reward",
   "out_of_creatures",
   "no_usable_move", "blocked",
]
## Engine messages the engine never says itself (C1: only effect logic's `say`, the chart's and tiers' messages and
## condition texts name them, so they are a game's choice): a manifest may leave them out, and then cannot name
## them (BattleData::messages holds nullptr: no text). The engine's Msg does not name them (C4); a battle may also
## declare messages of its own (game_messages()).
OPTIONAL_MESSAGES = ("super_effective", "not_very_effective", "hit_count", "confused", "confusion_hit",
                     "confusion_end", "became_confused", "already_confused", "flinched", "protected",
                     "protect_start", "recoil", "drained", "healed", "hp_full")
## The effect catalog's name of an optional message the manifest leaves out (not an identifier: `say` cannot name
## it; the list keeps every message at its Msg index).
ABSENT_MESSAGE = ""
## The messages every manifest gives (the engine says them itself), in Msg order.
REQUIRED_MESSAGES = [k for k in MESSAGES if k not in OPTIONAL_MESSAGES]
## Most messages in BattleData::messages (ids below kNone8: a tier's message is a uint8_t, kNone8 none).
MAX_MESSAGES = 255
## A message a battle declares itself (game_messages()): its key, a lower-case identifier (`say` names it).
MESSAGE_KEY = re.compile(r"[a-z][a-z0-9_]*\Z")
## Message keys that are gone, with what replaced them (refused with this migration message).
RETIRED_MESSAGES = {
   "no_pp": "the message no_pp is no_usable_move now (D-25: a move is usable when its cost can be paid; "
            "tools/battle_resources.py migrates a battle)",
}
## Concepts of the terms section and their neutral defaults (the one form; the many form follows plural_of()).
TERMS = {"party": "Party", "presenter": "Presenter", "creature": "Creature", "species": "Species", "move": "Move",
         "type": "Type", "category": "Category", "trait": "Trait", "passive": "Passive", "item": "Held item",
         "status": "Status", "weather": "Weather", "format": "Format", "stat": "Stat", "level": "Level",
         "exp": "EXP", "reward": "Reward", "growth": "Growth", "genes": "Genes", "training": "Training",
         "awaken": "Awakening"}
## Key of the terms section naming its plural rule (not a concept).
PLURAL_KEY = "plural"
## Plural rules a terms section may name (PLURAL_KEY): "english" (the default; plural_of()) derives a many form
## a concept does not give; "same" takes the one form (languages and games without plural forms).
PLURAL_RULES = ("english", "same")
## Plural rule of a terms section that names none.
DEFAULT_PLURAL_RULE = PLURAL_RULES[0]
## Last words whose English plural is not regular (lower case; the many form keeps the word's capitals).
IRREGULAR_PLURALS = {"species": "species", "series": "series", "genes": "genes", "training": "training",
                     "weather": "weather", "money": "money", "data": "data", "info": "info", "fish": "fish",
                     "sheep": "sheep", "deer": "deer", "child": "children", "person": "people", "man": "men",
                     "woman": "women", "mouse": "mice", "goose": "geese", "foot": "feet", "tooth": "teeth"}
## Endings that take "es" in English (box -> boxes, status -> statuses).
ES_ENDINGS = ("s", "x", "z", "ch", "sh")
## English vowels (a "y" after one takes a plain "s": day -> days).
VOWELS = "aeiou"
## Shortest all-capitals word read as an abbreviation that keeps its form (EXP, HP).
ABBREVIATION_CHARS = 2
## Concepts whose default many form is their one form (kept for the studio's TermsPanel; plural_of() decides).
INVARIANT_TERMS = {"species", "genes", "training", "exp", "weather"}
## Suffix of a regular many form (kept for the studio; plural_of() applies the English endings).
PLURAL_SUFFIX = "s"
## Longest term.
MAX_TERM_CHARS = 24
## Placeholders every message may use; MESSAGE_ARGS adds the one value a message carries.
MESSAGE_BASE_ARGS = {"user", "target"}
## The extra placeholder of each message (the value the engine passes with it).
MESSAGE_ARGS = {
   "party_challenge": {"party"}, "party_sends": {"party"}, "party_defeated": {"party"},
   "used_move": {"move"}, "learned": {"move"}, "not_learned": {"move"},
   "hit_count": {"value"}, "exp_gained": {"value"}, "level_up": {"value"}, "reward": {"value"},
   "stat_up": {"stat"}, "stat_up2": {"stat"}, "stat_down": {"stat"}, "stat_down2": {"stat"},
   "stat_max": {"stat"}, "stat_min": {"stat"}, "passive": {"passive"}, "item_used": {"item"},
}


def plural_of(word: str, rule: str = DEFAULT_PLURAL_RULE) -> str:
   """!The many form of a term the terms section gives only in its one form. "english": the last word's plural
   (IRREGULAR_PLURALS; an all-capitals abbreviation keeps its form; a consonant + y -> ies; ES_ENDINGS + es; else
   + s), so "Category" -> "Categories", "Status" -> "Statuses", "Held item" -> "Held items", "EXP" -> "EXP". "same":
   the word itself. The studio derives the plurals it shows with this function.
   @param word  the one form
   @param rule  one of PLURAL_RULES
   @return the many form
   """
   if rule != "english" or not word:
      return word
   cut = word.rfind(" ") + 1
   head, last = word[:cut], word[cut:]
   if len(last) >= ABBREVIATION_CHARS and last.isupper():
      return word
   lower = last.lower()
   if lower in IRREGULAR_PLURALS:
      # the irregular form, with the word's capital first letter
      many = IRREGULAR_PLURALS[lower]
      if last[:1].isupper():
         many = many[:1].upper() + many[1:]
      return head + many
   before_y = lower[-PAIR:-1]   # the letter before a final y ("" for a one-letter word)
   if lower.endswith("y") and before_y and before_y not in VOWELS:
      return head + last[:-1] + "ies"
   if lower.endswith(ES_ENDINGS):
      return head + last + "es"
   return head + last + PLURAL_SUFFIX


def compile_terms(src: Json, where: str) -> dict[str, dict[str, str]]:
   """!The terms section merged over the neutral defaults (TERMS); a concept without a many form gets
   plural_of() its one form by the section's plural rule ("plural": one of PLURAL_RULES, default english; the
   neutral defaults are English and always take the English rule).
   @param src    {concept: "Word" or {"one": "Word", "many": "Words"}, "plural": rule}
   @param where  location
   @return {concept: {"one": text, "many": text}} for every concept
   """
   if not isinstance(src, dict):
      raise ManifestError(f"{where}: must map concepts to words")
   unknown = sorted(set(src) - set(TERMS) - {PLURAL_KEY})
   if unknown:
      raise ManifestError(f"{where}: unknown concept(s) {', '.join(unknown)} (concepts: {', '.join(TERMS)})")
   rule = src.get(PLURAL_KEY, DEFAULT_PLURAL_RULE)
   if rule not in PLURAL_RULES:
      raise ManifestError(f"{where}.{PLURAL_KEY}: one of {', '.join(PLURAL_RULES)}")
   out = {}
   for concept, default in TERMS.items():
      v = src.get(concept, default)
      w = f"{where}.{concept}"
      if isinstance(v, str):
         v = {"one": v}
      if not isinstance(v, dict) or "one" not in v or set(v) - set(text_encoder.TERM_FORMS):
         raise ManifestError(f"{w}: a word, or {{\"one\": word, \"many\": words}}")
      one = v["one"]
      own_rule = rule if concept in src else DEFAULT_PLURAL_RULE
      many = v.get("many", plural_of(one, own_rule) if isinstance(one, str) else one)
      for form, word in (("one", one), ("many", many)):
         if not isinstance(word, str) or not 1 <= len(word) <= MAX_TERM_CHARS:
            raise ManifestError(f"{w}.{form}: 1-{MAX_TERM_CHARS} characters")
      out[concept] = {"one": one, "many": many}
   return out


def game_messages(msgs: Json) -> list[str]:
   """!The messages a manifest declares itself (C4): its keys that are not engine message keys (MESSAGES) nor
   retired ones, in the manifest's order. Effect logic (`say`), the chart and the condition records name them as
   they name the optional engine messages; the table holds them after MESSAGES.
   @param msgs  the manifest's messages {key: text}
   @return keys
   """
   given = msgs if isinstance(msgs, dict) else {}
   return [k for k in given if isinstance(k, str) and k not in MESSAGES and k not in RETIRED_MESSAGES]


def declared_messages(msgs: Json) -> list[str | None]:
   """!The message keys a manifest can name, at their message ids: the engine messages in Msg order
   (battlec.texts.MESSAGES; None where it leaves an optional message out, OPTIONAL_MESSAGES), then the messages it
   declares itself (game_messages()), so the index of a key is its id in BattleData::messages.
   @param msgs  the manifest's messages {key: text}
   @return keys or None
   """
   given = msgs if isinstance(msgs, dict) else {}
   return [None if k in OPTIONAL_MESSAGES and k not in given else k for k in MESSAGES] + game_messages(given)


def compile_messages(msgs: Json, where: str, enc: Texts,
                     extra: Mapping[str, set[str]] | None = None) -> list[list[int] | None]:
   """!The messages section: every required engine message (REQUIRED_MESSAGES) and the optional ones it gives, in
   MESSAGES order, then the ones it declares itself (game_messages(): a lower-case identifier each, its text with
   the {user} / {target} placeholders `say` gives), at most MAX_MESSAGES in all.
   @param msgs   {key: text}
   @param where  location
   @param enc    this compile's text encoders (with terms)
   @param extra  more placeholders some messages may use ({key: names}; G3: the recruit moments' {value}), or None
   @return encoded messages (None: an optional message left out)
   """
   extra = extra or {}
   if not isinstance(msgs, dict):
      raise ManifestError(f"{where}: messages must be an object")
   for key, why in RETIRED_MESSAGES.items():
      if key in msgs:
         raise ManifestError(f"{where}: {why}")
   own = game_messages(msgs)
   bad = [k for k in msgs if not isinstance(k, str) or (k in own and not MESSAGE_KEY.match(k))]
   if bad:
      raise ManifestError(f"{where}: message key(s) {', '.join(map(repr, bad))}: a message the battle declares "
                          f"itself needs a lower-case identifier for its key")
   missing = [k for k in REQUIRED_MESSAGES if k not in msgs]
   if missing:
      raise ManifestError(f"{where}: missing message(s) {', '.join(missing)}")
   if len(MESSAGES) + len(own) > MAX_MESSAGES:
      raise ManifestError(f"{where}: at most {MAX_MESSAGES - len(MESSAGES)} messages besides the engine's "
                          f"{len(MESSAGES)} (ids up to {MAX_MESSAGES - 1}; {len(own)} given)")
   return [enc.term_text(msgs[k], f"messages.{k}", MESSAGE_BASE_ARGS | MESSAGE_ARGS.get(k, set()) |
                         extra.get(k, set())) if k in msgs else None for k in MESSAGES + own]
