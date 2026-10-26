"""!Progression of the battle compiler: levels, growth (experience) tables,
party ranks and rewards; and (D-22 A8 / A12, D-23 E6c) the learn policy,
the experience policy, the generation list of new records (random ranges,
constants, passive slots, derived roles: rules::derive()). A move resource's
maximum (the former pp_max() slot) is checked by battlec/resources.py (D-25).

E13a: a wide random range draws its high half first ("order": "high_first",
the default) or its low half first ("low_first"); derived expressions may use
the ruleset language's bit operators and read the species' fields
(`species.NAME`, the structure's species_fields); an unstored custom role
(battlec/layout.py "stored": false) is computed on read from its derived or
constant entry. derived_values() / rederive() are the Python mirror of
battle::record_value() and battle::rederive() for tools (transfer imports, the
reference repo's record mappings).
"""
from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any

import ruleset_compiler as rc

from .common import PAIR, Json, ManifestError, check_keys, integer, register_keys
from .layout import PER_STAT_ROLES, RETIRED_ROLES, ROLE_NAMES, role_info, role_max_value, unstored_info
from .turn import MAX_PARTIES

## Rule default: highest party rank.
DEFAULT_RANK_MAX = 1
## Highest value one reward() may declare: six of them still add up inside int32.
MAX_REWARD = ((1 << 31) - 1) // MAX_PARTIES
## Highest experience (the Experience role is 24 bits).
MAX_EXPERIENCE = (1 << 24) - 1
## Lowest level_max.
MIN_LEVEL_MAX = 2
## Highest level_max (levels are 8-bit).
MAX_LEVEL_MAX = 255
## Rule default: highest level (neutral, D-22 decision 8: the engine's cap; a game states its own).
DEFAULT_LEVEL_MAX = MAX_LEVEL_MAX


def growth_names_of(growth_src: Json, where: str) -> list[str]:
   """!The growth section's table names (checked before the rules: the rules' level_max sizes the tables).
   @param growth_src  {name: expression in n}
   @param where       location
   @return names in manifest order
   """
   if not isinstance(growth_src, dict) or not growth_src:
      raise ManifestError(f"{where}: growth must map names to expressions")
   return list(growth_src)


def compile_growth(growth_src: Mapping[str, Any], growth_names: list[str], ruleset: Mapping[str, Any],
                   level_max: int, where: str) -> list[list[int]]:
   """!Growth tables (Experience role: 24 bits).
   @param growth_src    {name: expression in n}
   @param growth_names  growth_names_of() result
   @param ruleset       compiled ruleset (its constants)
   @param level_max     highest level
   @param where         location
   @return one experience table per growth (level_max + 1 entries)
   """
   growth = []
   for gname in growth_names:
      try:
         growth.append(rc.growth_table(growth_src[gname], ruleset["constants"], level_max,
                                       f"{where}: growth.{gname}", 0, MAX_EXPERIENCE))
      except rc.RulesetError as e:
         raise ManifestError(str(e))
   return growth


# -- progression rules (D-22 A8 / A12, D-23 E6c) ------------------------------------------------------------------

## learn.on_full values, in capsule::battle::LearnFull order.
LEARN_ON_FULL = ["skip", "request", "replace_oldest"]
## learn.default_moves values, in capsule::battle::DefaultMoves order.
DEFAULT_MOVE_RULES = ["last_n_by_level", "first_n"]
## exp_policy.share values, in capsule::battle::ExpShare order.
EXP_SHARES = ["faced", "active", "party"]
## exp_policy.receivers values, in capsule::battle::ExpReceivers order.
EXP_RECEIVERS = ["human_side", "opposing_sides"]
## exp_policy.outsider values, in capsule::battle::ExpOutsider order.
EXP_OUTSIDERS = ["never", "owner_id"]
## The learn policy's defaults (neutral, D-22 decision 8: a full creature learns nothing new). default_moves has
## no default: every manifest states which moves a creature made without a move list knows (REQUIRED_LEARN).
DEFAULT_LEARN = {"on_full": "skip"}
## learn keys a manifest must state.
REQUIRED_LEARN = ("default_moves",)
## The experience policy's defaults (neutral: the human side gains, no outsider bonus). share has no default:
## every manifest states who shares a fainted foe's experience (REQUIRED_EXP_POLICY).
DEFAULT_EXP_POLICY = {"receivers": "human_side", "outsider": "never"}
## exp_policy keys a manifest must state.
REQUIRED_EXP_POLICY = ("share",)
## Range bound: the role's highest value at its layout width.
BOUND_MAX = "max"
## Range bound: the last value the role chooses (trait: the last trait; passive_selector: the species' last
## rolled passive slot, chosen when the record is made).
BOUND_LAST = "last"
## The generation list without a manifest `generation` (neutral, D-22 decision 8: nothing is drawn; every role
## keeps its default value). The engine's former fixed draws were seed, stat_genes (0..max), trait and
## passive_selector (0..last), in that order; a game that wants them states them.
DEFAULT_GENERATION: list[dict[str, Any]] = []
## Generation entry kinds, in capsule::battle::GenKind order.
GEN_KINDS = ["random", "wide", "constant", "passive_slot", "derived", "wide_low_first"]
## Most draws per element of each kind (src/battle/progress.cpp generate()): random draws none for one value;
## passive_slot one, or none when the species fills no rolled slot past the first (rolled_passive_slot()).
DRAWS_PER_KIND = {"random": 1, "wide": 2, "constant": 0, "passive_slot": 1, "derived": 0, "wide_low_first": 2}
## A wide random range's draw orders (E13a; "order"): the kind each one compiles to.
WIDE_ORDERS = {"high_first": "wide", "low_first": "wide_low_first"}
## A wide random range's draw order without "order" (the engine's former only order).
DEFAULT_WIDE_ORDER = "high_first"
## Kinds that make no draw, the ones an unstored custom role may have (it is computed on read).
UNSTORED_KINDS = ("derived", "constant")
## Prefix of a species field's name in a derived expression (species.NAME, E13a).
SPECIES_PREFIX = "species."
## Most generation entries (capsule::battle::kMaxGeneration).
MAX_GENERATION = 32
## Roles make_monster() fills itself (no generation entry may set them; nor the resources' persist roles, which it
## fills with full pools: compile_generation(filled=)).
GENERATED_BY_ENGINE = ("species_key", "level", "experience", "moves")
## Most values one draw covers (rng::below(): 1 to 65536 outcomes).
ONE_DRAW_SPAN = 1 << 16
## Most values two draws cover (a 32-bit value).
TWO_DRAW_SPAN = 1 << 32
## Roles a "last" bound applies to.
LAST_ROLES = ("trait", "passive_selector")
## Bits of the low half a 32-bit role gives a derived expression (name_lo / name_hi).
HALF_BITS = 16
## Mask of a 16-bit half.
HALF_MASK = (1 << HALF_BITS) - 1
## Widest role a derived expression reads whole (int32 analysis; wider ones are read as halves).
MAX_WHOLE_BITS = 31
## Input of a derived expression: the element it is asked for.
DERIVED_ELEMENT = "element"
## Highest value of an 8-bit table entry (the species field table's element type).
BYTE_MAX = 0xFF
## Constant a derived expression may use: the number of traits.
TRAIT_COUNT = "TRAIT_COUNT"

register_keys({
   "learn": {"on_full", "default_moves"},
   "exp_policy": {"share", "receivers", "outsider"},
   "generation_entry": {"role", "random_range", "constant", "derived", "range", "order"},
})


def choice(v: Json, options: Sequence[str], where: str) -> int:
   """!One of a fixed set of words.
   @param v        value
   @param options  the words, in engine enum order
   @param where    location
   @return its index
   """
   if v not in options:
      raise ManifestError(f"{where}: must be one of {', '.join(options)}, got {v!r}")
   return options.index(v)


def compile_learn(src: Json, where: str) -> dict[str, int]:
   """!The learn policy (manifest `learn`: REQUIRED_LEARN must be given, other missing keys take DEFAULT_LEARN's).
   @param src    the manifest's learn (or None)
   @param where  location
   @return {"on_full": LearnFull, "default_moves": DefaultMoves}
   """
   src = {} if src is None else src
   w = f"{where}: learn"
   check_keys(src, "learn", w)
   for key in REQUIRED_LEARN:
      if key not in src:
         raise ManifestError(f"{w}.{key}: required (one of {', '.join(DEFAULT_MOVE_RULES)}; no default)")
   src = {**DEFAULT_LEARN, **src}
   return {"on_full": choice(src["on_full"], LEARN_ON_FULL, f"{w}.on_full"),
           "default_moves": choice(src["default_moves"], DEFAULT_MOVE_RULES, f"{w}.default_moves")}


def compile_exp_policy(src: Json, layout: Mapping[str, Any], where: str) -> dict[str, int]:
   """!The experience policy (manifest `exp_policy`: REQUIRED_EXP_POLICY must be given, other missing keys take
   DEFAULT_EXP_POLICY's). The exp_gain() formula itself is the ruleset's slot.
   @param src     the manifest's exp_policy (or None)
   @param layout  compiled layout (outsider "owner_id" needs the owner_id role)
   @param where   location
   @return {"share": ExpShare, "receivers": ExpReceivers, "outsider": ExpOutsider}
   """
   src = {} if src is None else src
   w = f"{where}: exp_policy"
   check_keys(src, "exp_policy", w)
   for key in REQUIRED_EXP_POLICY:
      if key not in src:
         raise ManifestError(f"{w}.{key}: required (one of {', '.join(EXP_SHARES)}; no default)")
   src = {**DEFAULT_EXP_POLICY, **src}
   out = {"share": choice(src["share"], EXP_SHARES, f"{w}.share"),
          "receivers": choice(src["receivers"], EXP_RECEIVERS, f"{w}.receivers"),
          "outsider": choice(src["outsider"], EXP_OUTSIDERS, f"{w}.outsider")}
   if EXP_OUTSIDERS[out["outsider"]] == "owner_id" and role_info(layout, "owner_id") is None:
      raise ManifestError(f"{w}.outsider: \"owner_id\" compares the owner_id role, which the layout does not hold")
   return out


def generation_role(name: Json, layout: Mapping[str, Any], where: str,
                    filled: Sequence[str] = ()) -> tuple[str, int, int, int]:
   """!The role a generation entry fills.
   @param name    role name (an engine role, held by the layout or not, or one of the layout's custom roles)
   @param layout  compiled layout
   @param where   location
   @param filled  the resources' persist roles (make_monster() fills them with full pools)
   @return (name, RoleId, element count, highest value)
   """
   if not isinstance(name, str):
      raise ManifestError(f"{where}.role: names a record role")
   engine = list(GENERATED_BY_ENGINE) + list(filled)
   if name in engine:
      raise ManifestError(f"{where}.role: {name!r} is set by the engine when it makes a record "
                          f"({', '.join(engine)})")
   info = role_info(layout, name) or unstored_info(layout, name)
   if info is not None:
      rid, _width, count = info
   elif name in ROLE_NAMES and name not in RETIRED_ROLES:
      # an engine role the layout leaves out: it still draws (nothing is stored)
      rid = ROLE_NAMES.index(name)
      count = layout["counts"][rid]
   else:
      raise ManifestError(f"{where}.role: {name!r} is neither an engine role nor a custom role of the layout")
   return name, rid, count, role_max_value(layout, name)


def bound(v: Json, role: str, top: int, traits: int, where: str) -> int | str:
   """!One bound of a random range.
   @param v       an integer, BOUND_MAX or BOUND_LAST
   @param role    the role
   @param top     the role's highest value
   @param traits  traits in the data
   @param where   location
   @return the value, or BOUND_LAST for the species' passive slots
   """
   if v == BOUND_MAX:
      return top
   if v == BOUND_LAST:
      if role not in LAST_ROLES:
         raise ManifestError(f"{where}: {BOUND_LAST!r} is the last value of {' or '.join(LAST_ROLES)}")
      return traits - 1 if role == "trait" else BOUND_LAST
   return integer(v, 0, top, where)


def derived_inputs(layout: Mapping[str, Any]) -> dict[str, tuple[int, int, int, bool]]:
   """!The role names a derived expression reads: the layout's single-element roles (32-bit ones as NAME_hi /
   NAME_lo halves; DERIVED_ELEMENT is the other input).
   @param layout  compiled layout
   @return name -> (RoleId, shift, highest value, True for a half)
   """
   out = {}
   names = [n for n in ROLE_NAMES if n not in layout["custom_ids"]] + list(layout["custom_ids"])
   for name in names:
      info = role_info(layout, name)
      if info is None:
         continue
      rid, width, count = info
      if count != 1:
         continue
      if width > MAX_WHOLE_BITS:
         out[f"{name}_hi"] = (rid, HALF_BITS, HALF_MASK, True)
         out[f"{name}_lo"] = (rid, 0, HALF_MASK, True)
      else:
         out[name] = (rid, 0, (1 << width) - 1, False)
   return out


def species_inputs(species_fields: Sequence[Mapping[str, Any]]) -> dict[str, tuple[int, int]]:
   """!The species fields a derived expression reads (E13a: `species.NAME`) and their value ranges.
   @param species_fields  the compiled species_fields (battlec/content.py compile_species_fields())
   @return name (with SPECIES_PREFIX) -> (lo, hi)
   """
   return {SPECIES_PREFIX + f["name"]: (0, f["max"]) for f in species_fields}


def compile_generation(src: Json, layout: Mapping[str, Any], ruleset: Mapping[str, Any], rules: Mapping[str, Any],
                       traits: int, where: str, filled: Sequence[str] = (),
                       species_fields: Sequence[Mapping[str, Any]] = ()) -> list[dict[str, Any]]:
   """!The generation list (manifest `generation`; DEFAULT_GENERATION without one). Its order is the draw order.
   Entries: {"role", "random_range": [lo, hi], "order"} (one draw per element below 65536 values, two draws for a
   power of two above: the high half first, or with "order": "low_first" the low half first; one value draws
   nothing; hi may be "max" or, for trait and passive_selector, "last"), {"role", "constant": v} or {"role",
   "derived": expression, "range": [lo, hi]} (over the layout's stored single-element roles, 32-bit ones as
   NAME_hi / NAME_lo, "element", the species' fields as species.NAME, the ruleset's constants, STAT_CAP,
   LEVEL_MAX and TRAIT_COUNT; the ruleset language with its bit operators; default range the role's values).
   An unstored custom role (computed on read) has exactly one entry, derived or constant.
   @param src             the manifest's generation (or None)
   @param layout          compiled layout
   @param ruleset         compiled ruleset (its constants)
   @param rules           compiled rules (the battle constants)
   @param traits          traits in the data
   @param where           location
   @param filled          the resources' persist roles (no entry may set them)
   @param species_fields  the compiled species_fields (their values: battlec/content.py)
   @return entries {"role", "id", "kind" (GenKind), "value", "span", "count", "derived" (index or 0),
           "formula" (derived: compiled tree, inputs, range)}
   """
   entries = DEFAULT_GENERATION if src is None else src
   w = f"{where}: generation"
   if not isinstance(entries, list) or len(entries) > MAX_GENERATION:
      raise ManifestError(f"{w}: a list of at most {MAX_GENERATION} entries")
   constants = {**ruleset["constants"], **{k: rules[v] for k, v in rc.BATTLE_CONSTANTS.items()}}
   if TRAIT_COUNT in constants:
      raise ManifestError(f"{w}: the ruleset's constant {TRAIT_COUNT} clashes with the generation's own")
   constants[TRAIT_COUNT] = traits
   inputs = derived_inputs(layout)
   fields = species_inputs(species_fields)
   out = []
   derived = 0
   for k, e in enumerate(entries):
      ew = f"{w}[{k}]"
      check_keys(e, "generation_entry", ew)
      ways = [x for x in ("random_range", "constant", "derived") if x in e]
      if len(ways) != 1:
         raise ManifestError(f"{ew}: give exactly one of random_range, constant or derived")
      if "range" in e and ways[0] != "derived":
         raise ManifestError(f"{ew}.range: only a derived entry declares its range")
      if "order" in e and ways[0] != "random_range":
         raise ManifestError(f"{ew}.order: only a wide random_range has a draw order")
      name, rid, count, top = generation_role(e.get("role"), layout, ew, filled)
      entry = {"role": name, "id": rid, "count": count, "value": 0, "span": 0, "derived": 0}
      if ways[0] == "constant":
         entry.update(kind=GEN_KINDS.index("constant"), value=integer(e["constant"], 0, top, f"{ew}.constant"))
      elif ways[0] == "random_range":
         rng = e["random_range"]
         if not (isinstance(rng, list) and len(rng) == PAIR):
            raise ManifestError(f"{ew}.random_range: [lo, hi]")
         lo = bound(rng[0], name, top, traits, f"{ew}.random_range[0]")
         hi = bound(rng[1], name, top, traits, f"{ew}.random_range[1]")
         if hi == BOUND_LAST:
            if lo != 0:
               raise ManifestError(f"{ew}.random_range: the passive slots start at 0")
            entry.update(kind=GEN_KINDS.index("passive_slot"))
         elif name == "trait" and rng[1] == BOUND_LAST and traits == 0:
            continue   # no traits: nothing to draw (the former rule)
         else:
            if lo == BOUND_LAST or lo > hi:
               raise ManifestError(f"{ew}.random_range: [lo, hi] with lo <= hi")
            span = hi - lo + 1
            if span <= ONE_DRAW_SPAN:
               entry.update(kind=GEN_KINDS.index("random"), value=lo, span=span)
            elif span & (span - 1) == 0 and span <= TWO_DRAW_SPAN:
               order = e.get("order", DEFAULT_WIDE_ORDER)
               if order not in WIDE_ORDERS:
                  raise ManifestError(f"{ew}.order: one of {', '.join(WIDE_ORDERS)}, got {order!r}")
               entry.update(kind=GEN_KINDS.index(WIDE_ORDERS[order]), value=lo, span=span - 1)
            else:
               raise ManifestError(f"{ew}.random_range: more than {ONE_DRAW_SPAN} values must be a power of two "
                                   f"of them (two draws), got {span}")
            if "order" in e and GEN_KINDS[entry["kind"]] not in WIDE_ORDERS.values():
               raise ManifestError(f"{ew}.order: only a wide random_range (two draws) has a draw order")
      else:
         lo, hi = (0, top) if "range" not in e else rc.check_range(e["range"], constants, f"{ew}.range")
         if lo < 0 or hi > top:
            raise ManifestError(f"{ew}.range: must lie within [0, {top}] (the role's values)")
         try:
            ranges = {n: (0, top_value) for n, (_rid, _shift, top_value, _half) in inputs.items()}
            ranges.update(fields)
            ranges[DERIVED_ELEMENT] = (0, max(count - 1, 0))
            tree = rc.parse(e["derived"], list(ranges), constants, f"{ew}.derived")
            bounds = rc.interval(tree, ranges, constants, f"{ew}.derived", [])
         except rc.RulesetError as err:
            raise ManifestError(str(err))
         entry.update(kind=GEN_KINDS.index("derived"), derived=derived,
                      formula={"tree": tree, "expr": e["derived"], "range": (lo, hi), "bounds": bounds,
                               "constants": constants, "used": sorted(rc.used_inputs(tree))})
         derived += 1
      out.append(entry)
   check_unstored(out, layout, w)
   return out


def check_unstored(entries: Sequence[Mapping[str, Any]], layout: Mapping[str, Any], where: str) -> None:
   """!Checks that every unstored custom role (computed on read, E13a) has exactly one generation entry, derived or
   constant (a draw would be lost: nothing holds it).
   @param entries  compile_generation() entries
   @param layout   compiled layout
   @param where    location
   @return None
   @throws ManifestError naming the role
   """
   for u in layout.get("unstored", []):
      kinds = [GEN_KINDS[e["kind"]] for e in entries if e["role"] == u["name"]]
      if len(kinds) != 1 or kinds[0] not in UNSTORED_KINDS:
         raise ManifestError(f"{where}: the unstored role {u['name']!r} is computed on read: give it exactly one "
                             f"{' or '.join(UNSTORED_KINDS)} entry (it has {', '.join(kinds) or 'none'})")


def emit_generation(entries: Sequence[Mapping[str, Any]]) -> list[str]:
   """!The kGeneration table (capsule::battle::GenData) of a compiled generation list (one zero entry when it is
   empty, which the count leaves out).
   @param entries  compile_generation() result
   @return C++ lines
   """
   L = ["const GenData kGeneration[] = {"]
   for e in entries:
      L.append(f"    {{{e['value']}u, {e['span']}u, {e['id']}, {e['kind']}, {e['count']}, {e['derived']}}},"
               f"   // {e['role']}: {GEN_KINDS[e['kind']]}")
   if not entries:
      L.append("    {0u, 0u, 0, 0, 0, 0},")
   L.append("};")
   return L


def emit_species_fields(species: Sequence[Mapping[str, Any]], species_fields: Sequence[Mapping[str, Any]],
                        entries: Sequence[Mapping[str, Any]], logic_reads: bool = False) -> list[str]:
   """!The species field table (kSpeciesFields: one row per species in key order, the build's species indices; E13a),
   species_field() for derived expressions (only when one reads a field: no unused function) and the public
   species_field_of() effect logic reads (E13b), inside namespace capsule::rules. The table is there only when a
   derived expression or effect logic reads a field (else species_field_of() returns 0: no table in ROM).
   @param species         compiled species (battlec/content.py: each with "fields")
   @param species_fields  compiled species_fields
   @param entries         compile_generation() result
   @param logic_reads     whether effect logic reads a species field (E13b)
   @return C++ lines
   """
   used = any(n.startswith(SPECIES_PREFIX) for e in entries if GEN_KINDS[e["kind"]] == "derived"
              for n in e["formula"]["used"])
   # E13b: effect logic reads the fields through species_field_of() (0 for a structure without fields)
   reader = ["/// A species field of a species (effect logic's `species_field(NAME)`; E13b).",
             "/// @param species  species index", "/// @param field    species_fields index", "/// @return the value",
             "int32_t species_field_of(int32_t species, int32_t field) {"]
   if not species_fields or not (used or logic_reads):
      return reader + ["    (void)species;", "    (void)field;", "    return 0;   // no species field is read", "}",
                       ""]
   top = max(f["max"] for f in species_fields)
   ty = "uint8_t" if top <= BYTE_MAX else "uint16_t" if top <= HALF_MASK else "uint32_t"
   L = ["namespace {", "", "/// Number of species fields (structure species_fields).",
        f"constexpr int kSpeciesFieldCount = {len(species_fields)};",
        "/// Each species' fields (rows: the species in key order; columns: species_fields).",
        f"constexpr {ty} kSpeciesFields[][kSpeciesFieldCount] = {{"]
   for sp in species:
      L.append(f"    {{{', '.join(str(sp['fields'][f['name']]) for f in species_fields)}}},   // {sp['name']}")
   if not species:
      L.append(f"    {{{', '.join(str(f['default']) for f in species_fields)}}},")
   L += ["};",
         "/// Each species field's value for a record whose species is not in the data.",
         f"constexpr {ty} kSpeciesFieldDefaults[kSpeciesFieldCount] = "
         f"{{{', '.join(str(f['default']) for f in species_fields)}}};", ""]
   if used:
      L += ["/// A species field of a record's species (its default when the data has no such species).",
            "/// @param rec    the record (its species_key role)",
            "/// @param field  species_fields index",
            "/// @return the value",
            "int32_t species_field(const monster::Record& rec, int field) {",
            "    const uint16_t sp = battle::find_species(AssetKey{monster::get(rec, monster::Role::SpeciesKey)});",
            f"    return static_cast<int32_t>(sp < {len(species)} ? kSpeciesFields[sp][field] : "
            "kSpeciesFieldDefaults[field]);",
            "}", ""]
   L += ["} // namespace", ""] + reader + [
      "    if (field < 0 || field >= kSpeciesFieldCount) { return 0; }",
      f"    return static_cast<int32_t>(species >= 0 && species < {len(species)} ? kSpeciesFields[species][field] : "
      "kSpeciesFieldDefaults[field]);",
      "}", ""]
   return L


def emit_derive(entries: Sequence[Mapping[str, Any]], layout: Mapping[str, Any],
                species_fields: Sequence[Mapping[str, Any]] = ()) -> list[str]:
   """!C++ definition of capsule::rules::derive() (inside namespace capsule::rules): one case per derived entry
   (species fields through species_field(), emit_species_fields()).
   @param entries         compile_generation() result
   @param layout          compiled layout
   @param species_fields  compiled species_fields
   @return C++ lines
   """
   inputs = derived_inputs(layout)
   field_index = {SPECIES_PREFIX + f["name"]: k for k, f in enumerate(species_fields)}
   derived = [e for e in entries if GEN_KINDS[e["kind"]] == "derived"]
   L = ["int32_t derive(int32_t index, int32_t element, const monster::Record& rec) {"]
   if not derived:
      L += ["    (void)index;", "    (void)element;", "    (void)rec;", "    return 0;", "}"]
      return L
   L.append("    switch (index) {")
   for e in derived:
      f = e["formula"]
      L.append(f"    case {e['derived']}: {{   // {e['role']} = {f['expr']}")
      names = {}
      for n in f["used"]:
         if n == DERIVED_ELEMENT:
            continue
         if n in field_index:
            names[n] = f"species_field(rec, {field_index[n]})"
            continue
         rid, shift, mask, half = inputs[n]
         get = f"monster::get(rec, monster::RoleId{{{rid}}}, 0)"
         whole = f"static_cast<int32_t>({get})"
         names[n] = f"static_cast<int32_t>(({get} >> {shift}) & 0x{mask:X}u)" if half else whole
      if DERIVED_ELEMENT not in f["used"]:
         L.append("        (void)element;")
      lo, hi = f["range"]
      L.append(f"        return rules::clamp32({rc.to_cpp(f['tree'], f['constants'], names)}, int32_t{{{lo}}}, "
               f"int32_t{{{hi}}});")
      L.append("    }")
   L += ["    default:", "        (void)element;", "        (void)rec;", "        return 0;", "    }", "}"]
   return L


def run_derived(entry: Mapping[str, Any], values: Mapping[str, int], element: int = 0) -> int:
   """!Evaluates a derived entry as the generated rules::derive() would (tools and tests).
   @param entry    a derived entry of compile_generation()
   @param values   input name -> value (the layout's roles, as derived_inputs() names them; species fields as
                   species.NAME)
   @param element  the element
   @return the value, clamped to the entry's range
   """
   f = entry["formula"]
   v = rc.evaluate(f["tree"], {**values, DERIVED_ELEMENT: element}, f["constants"])
   return max(f["range"][0], min(f["range"][1], v))


def record_inputs(compiled: Mapping[str, Any], record: Mapping[str, Any]) -> dict[str, int]:
   """!The inputs a derived expression sees for a record dict (tools/record_transfer.py unpack() form): the layout's
   stored single-element roles (32-bit ones as halves) and the record's species' fields.
   @param compiled  battle_compiler.compile_battle() result
   @param record    record dict (roles left out read 0)
   @return input name -> value
   """
   values = {}
   for name, (_rid, shift, mask, half) in derived_inputs(compiled["layout"]).items():
      role = name.rsplit("_", 1)[0] if half else name
      values[name] = (int(record.get(role, 0)) >> shift) & mask
   fields = compiled.get("species_fields", [])
   by_key = {sp["key"]: sp for sp in compiled["species"]}
   sp = by_key.get(int(record.get("species_key", 0)))
   for f in fields:
      values[SPECIES_PREFIX + f["name"]] = sp["fields"][f["name"]] if sp is not None else f["default"]
   return values


def derived_values(compiled: Mapping[str, Any], record: Mapping[str, Any]) -> dict[str, Any]:
   """!Every derived entry's value for a record dict, as battle::record_value() reads it (an unstored role, a role
   the layout leaves out) or battle::rederive() writes it (a stored one): in generation order, each entry seeing
   the stored roles as the record holds them.
   @param compiled  battle_compiler.compile_battle() result
   @param record    record dict
   @return role -> value (a list for a role of several elements)
   """
   values = record_inputs(compiled, record)
   out: dict[str, Any] = {}
   for e in compiled["rules"]["progress"]["generation"]:
      if GEN_KINDS[e["kind"]] != "derived":
         continue
      out[e["role"]] = _shaped(compiled, e, [run_derived(e, values, k) for k in range(max(e["count"], 1))])
   return out


def _shaped(compiled: Mapping[str, Any], entry: Mapping[str, Any], got: Sequence[int]) -> Any:
   """!Element values in a record dict's form (tools/record_transfer.py unpack()).
   @param compiled  battle_compiler.compile_battle() result
   @param entry     the generation entry
   @param got       its values, by element
   @return an int (one element), {stat name: value} (a per-stat role) or a list
   """
   if entry["count"] == 1:
      return got[0]
   if entry["role"] in PER_STAT_ROLES:
      return {st["name"]: v for st, v in zip(compiled["stats"], got)}
   return list(got)


def rederive(compiled: Mapping[str, Any], record: Mapping[str, Any], keep: Sequence[str] = ()) -> dict[str, Any]:
   """!A record dict whose stored derived roles follow its other roles again (battle::rederive(): each derived entry
   in generation order, later ones reading the roles re-derived before them). Unstored roles need none: they are
   computed on read.
   @param compiled  battle_compiler.compile_battle() result
   @param record    record dict
   @param keep      roles to leave as they are (an import keeps the values the sender gave)
   @return a new record dict
   """
   out = dict(record)
   layout = compiled["layout"]
   for e in compiled["rules"]["progress"]["generation"]:
      if GEN_KINDS[e["kind"]] != "derived" or e["role"] in keep or role_info(layout, e["role"]) is None:
         continue
      values = record_inputs(compiled, out)
      out[e["role"]] = _shaped(compiled, e, [run_derived(e, values, k) for k in range(e["count"])])
   return out
