# Starter sample

The engine's neutral sample game (D-25, E9) and reference fixture: nine stats with HP, MP and EP pools, physical
moves paying EP, elemental and status moves paying MP, Intelligence deciding effect odds, nine types, nine invented
creatures, talents, charms, three rivals and shape art. New games copy it (the engine README, "Starting a new game
from the Starter sample"; the studio's "Starter" template copies the files `template.json` lists).

## Starter rules

The Starter's numbers and formulas are its own (C2, decided 2026-10-19): it borrows no stage table, odds, factor or
formula shape from another game. Each rule below is data in this folder, so a game that copies the sample changes
it here. In `ruleset.json` every number is a named constant (`constants`), and `editor.notes` summarises the
formulas.

| Rule | Number or formula | Where | Why |
| --- | --- | --- | --- |
| Level cap | 50 | `structure.json` `rules.level_max` | a short game; the growth curves stay small (16 200 / 21 600 EXP) |
| Stat cap | 700 | `rules.stat_cap` | just above the highest HP the formula reaches (654); every stat formula stays within it |
| Staged stats | up to 1400 (`STAGED_CAP`: the stat cap x the top stage's x2) | `ruleset.json` `constants`, the `attack` / `defense` / `speed` lets, the `offense` point, the inputs `attack`, `defense`, `speed`, `foe_speed` | a staged stat is not clamped at the stat cap, so +3 and +4 still count at the top end (C4) |
| Genes | 0..15 per stat | `stat_genes` role, 4 bits | unchanged record layout |
| HP (`vital`) | `10 + base / 8 + level * (base + gene) / 22 + training / 8` (10..658) | `ruleset.json` `stat_formulas` | a small head start from the base, then linear growth: low levels are not one-hit |
| MP, EP (`reserve`) | `6 + level * (base + gene) / 30 + training / 8` (6..460) | `stat_formulas` | pools sized for 2-4 casts at low levels; no temperament |
| Attack .. Agility (`core`) | `(3 + base / 16 + level * (base + gene) / 36) * temperament / 100 + training / 8` (2..455) | `stat_formulas` | the same shape as HP with a smaller head start |
| Temperaments | raised stat x1.15, lowered stat x0.88 | `battle.json` `traits[].pct` | a slight net gain, so no temperament is a pure loss |
| Damage | `Attack * power * (level + 5) / ((Defence + 10) * 50) + 1` | `formulas.damage_base` | attack against padded defence: a low Defence does not multiply damage without bound |
| Damage order | base, crit, same type, chart, spread, weather point, roll, damage / guard points, at least 1 | `pipelines.damage` | the roll comes last so every factor is rolled together |
| Damage roll | 17 outcomes, x0.92 .. x1.08 (`ROLL_LOW` 92, `ROLL_SPAN` 17, `ROLL_DEN` 100) | `pipelines.damage` | a narrow roll, centred on x1 |
| Critical hit | x1.625 (`CRIT` 416 / 256); uses the target's Defence without its stages, the user's stages apply | `pipelines.damage` | a crit pierces the target's guard; boosts the user earned still count |
| Crit odds | stage 0..3: 1/20, 1/10, 1/5, 3/10 | `structure.json` `stages` (critical) | rare by default, a real choice at the top stage |
| Same type | x1.2 (`SAME_TYPE` 307 / 256) | `pipelines.damage` | a small bonus, so off-type moves stay useful |
| Spread | x0.7 (`SPREAD` 180 / 256) when a move hits more than one target | `pipelines.damage` | |
| Type chart | strong x1.75 (448), weak x0.6 (154), none x0 (decisive, "no effect"), `multiply` | `structure.json` `chart.tiers` | double strong x3.06, strong and weak x1.05, double weak x0.36 |
| Stat stages | -4..+4: x0.40, 0.55, 0.70, 0.85, 1, 1.25, 1.50, 1.75, 2.00 | `stages` (one shared table) | linear both ways: +25 % a stage up, -15 % a stage down |
| Accuracy | -4..+4: x0.6 .. x1.4, 10 % a stage, of accuracy stage minus evasion stage (clamped) | `stages` (accuracy, evasion) | |
| Self-hit | power 30, physical, the damage formula and roll without crit or chart | `rules.self_hit`, `pipelines.self_hit` | |
| Dazed | 1-3 moves; each move, 1 in 4 hits itself (power 30) | `volatiles`, `effects/conditions.ceffect` | |
| Messages | the engine's required keys, the optional `super_effective`, `not_very_effective`, `drained`, `healed`, `hp_full`, and its own `dazed`, `daze_hit`, `daze_end`, `became_dazed`, `already_dazed`, `staggered` | `battle.json` `messages` | a battle declares the messages its logic says itself (C4): neutral keys, no borrowed ones |
| Effect odds | `chance * (2 * user Int + 60) / (user Int + target Int + 60)`, then the `odds` / `resist` points | `pipelines.effect_odds` | equal Intelligence keeps the odds asked for; a clever user at most doubles them, a clever target drives them toward 0 |
| EP regen | `1 + (EP max - EP) / 16` each `regen` phase; MP does not regenerate | `formulas.ep_regen`, `resources` | small, and faster when the pool is nearly empty |
| Growth | brisk `6*n*n + 24*n`, steady `8*n*n + 32*n` (EXP to reach level n) | `structure.json` `growth` | quadratic: later levels cost more, but not steeply |
| Experience | `max(1, yield * foe level / 10 * (foe level + 10) / (own level + 10) * (4 + rank) / 4 / participants)` | `formulas.exp_gain` | the level difference counts: a stronger foe gives more, a weaker one less; a rival x1.25 |
| Reward | `base * (level + 8) / 2` (level: the rival's last creature) | `formulas.reward` | |
| Escape | out of 256: `96 + 128 * speed / (speed + foe speed) + 48 * failed attempts` | `formulas.escape_odds` | a share of the combined speed: never hopeless, never certain at first |
| Scorch | 1/12 of max HP a turn (21 / 256); the scorched battler's physical damage x0.7 | `statuses`, `conditions.ceffect` | |
| Blight | 1/10 of max HP a turn (26 / 256) | `statuses` | |
| Shock | loses 1 move in 5; speed x0.7 | `statuses`, `conditions.ceffect` | |
| Doze | 1-3 moves | `statuses` | |
| Storm | 5 turns; Lightning x1.3, Fire x0.7 | `weathers`, `conditions.ceffect` | |
| Talents | Blaze Heart Fire x1.4 at 2/5 HP or less; Quick Feet speed x1.3 at half HP or less; Thick Hide physical damage taken x0.75; Insight odds x1.5; Stubborn odds against it x0.5 | `effects/passives.ceffect` | |
| Charms | Focus Lens odds x1.25; Mana Drop +20 MP and Vigor Bar +20 EP at a quarter or less | `effects/items.ceffect` | |
| Bag (G2) | 4 pockets: Supplies 20 slots of up to 99 (as added), Orbs 8 of 99 (by index), Charms 16 of 9 (by name), Key 16 of 1 | `structure.json` `pockets` | the D-26 decision 2 pockets; the GBA holds 256 slots, the Starter uses 60 |
| Supplies | Tonic +20 HP (out of battle and, from G3, in battle), Mana Tea +15 MP, Spark Snack +15 EP, Remedy cures the status; a full pool, a fainted member or no status: no effect, nothing used | `battle.json` `items`, `effects/items.ceffect` (`use_field`) | small, cheap restores; the charms stay held items |
| Prices and shops | Tonic 30, Mana Tea 40, Spark Snack 25, Remedy 50, Orb 20 (recruits, G3), Mana Drop / Vigor Bar 120, Focus Lens 200; Gate Pass is a key item; Corner Shop (supplies, Orb), Charm Stall (charms) | `items[].price`, `shops` | |
| Transformations (G4) | each of the nine first-stage creatures becomes its second stage at level 20 (level_up; the player may stop it); Sproutle becomes Sprigshield with a Bloom Charm (item_use); the second stages' bases are x1.25 (capped 255), yields doubled | `battle.json` `species[].transforms`, `structure.json` `transformation` | D-26 decision 2: one two-stage line per type by level and one by item |
| Forms (G4) | Glimmet's Radiant form (Light / Cosmic, Magic and Intelligence bases +12, its own sprites): after a battle at night, and the Glow Charm (a key item) toggles it | `species[].forms`, `transformation.form_role` (`form`, 1 bit), `effects/items.ceffect` | one creature with two forms |
| Training (G4) | each defeated foe gives `2 * yield` to the stats it yields (HP, Attack, Magic or Agility: 1 for a first stage, 2 for a second), at most 32 per stat and 96 in all; a stat gains `training / 8`; Drill Notes +4 Attack (to 16 of its own) | `structure.json` `training`, `formulas.train_gain`, `species_fields` `yield_*`, the `stat_training` role (6 bits) | a small, visible reward for battling, never more than +4 a stat |
| Recruiting (G3) | an Orb (Orbs pocket, used in battle on a foe) makes 3 checks, each passing at `recruit_rate * (100 + missing HP %) / 400 + Orb bonus 5 + status bonus` percent (clamped 0..100; a bonus of 100 or more is sure); status bonus Doze 25, Shock / Scorch / Blight 10; HP and status kept; a rival's creature is refused, the Orb kept | `structure.json` `recruit`, `ruleset.json` `pipelines.recruit_odds` (`RECRUIT_RATE_DIV`, `RECRUIT_SURE`), `species_fields` `recruit_rate` (default 100), `effects/items.ceffect` (Orb) | neutral odds: a weakened or dozing creature is easier, never certain without a sure bonus |
| Storage (G3) | 8 boxes of 28 (B2: 30 before; the Grove's nursery section keeps two copies of the image in the GBA's 32 KB SRAM), `Box 1` .. `Box 8`; a recruit with a full party goes to the current box | `structure.json` `storage` | 224 slots, 14 KB of the save image |
| AI items (G3) | Strategist uses a Tonic below a quarter of its HP (rule `below_hp`); Scholar Wen carries one | `ai_profiles[].items`, `parties[].items` | the clever rival heals once |
| AI | profiles (`ai_profiles`, G1): Basic (the default: noise, status_move), Battler (adds damage, avoid_useless) and Strategist (adds status and save_resources: prefers cheaper moves); Novice Kit and the duo name Battler, Scholar Wen names Strategist | `battle.json` `ai_profiles`, `parties[].ai` | the clever rival spends its MP carefully, the novice does not |
| Field rules (F1) | the lead is the first member with HP (the structure has no `lead` rule); the Ward charm (Charms, 300 coins, used up) starts the timed field effect `ward` for 120 steps (used again: it starts over; it survives a save; "The Ward charm's glow fades." when it ends), and while it runs nothing below the lead's level is met (the global `ward_rule`); Quick Feet leading the party: encounter rate x3/2; the encounter pipelines are the engine's defaults (the table's odds x the hooks' scale, a level drawn between the entry's bounds) | `structure.json` `field_effects`, `battle.json` `items`, `effects/field.ceffect`, `effects/passives.ceffect` (Quick Feet `on encounter`) | D-27 decision 3: a ward that keeps the weak away but not the strong, a talent that only matters at the front |
| Field actions (F2) | three actions, each asking first: `wade` (a member that knows Tide Slam or Torrent, facing the world's `water` behavior: "The water is calm. Wade across?", then the game's script, e.g. `vehicle swim;` and a step), `clear` (Vine Lash or Spore Burst, facing a `bush` object: "A thick bush blocks the way. Clear it?", e.g. `hide self;`), `light` (Mend Light or Prism Beam, a menu action on maps tagged `dark`: "It is dark here. Light the way?", e.g. `darkness 0;`) (C8: the darkness levels are the world's `darkness_levels`, the sandbox's 72 / 56 / 40 / 24 pixel lit boxes at brightness 6 / 10 / 13 / 16; a script-set level is kept across a save until the next map); the first member in party order that qualifies uses it; no badge or story flag is required | `structure.json` `field_actions` (the scripts `field_wade`, `field_clear`, `field_light`, the behavior `water`, the object kind `bush` and the map tag `dark` come from the game's scripts and world: apps/sandbox_map) | D-27 decision 3: cross water through a vehicle class, clear a bush object, light a dark cave |
| Step effects (F3) | two step effects run on the party as the walker walks: `blight` every 4 steps (a Blighted member loses 1 HP; at 0 the survive rule keeps it at 1 and the blight fades: "{user} held on at 1 HP! The blight faded.") and `bond` every 64 steps (every member's custom role `bond`, 8 bits, grows by 1, at most 255: the global `bond_rule`); the counters are saved (implicit script variables) | `structure.json` `step_effects` and `monster_layout` (`bond`), `battle.json` `messages` (`blight_survived`), `effects/conditions.ceffect` (Blight `on steps`), `effects/field.ceffect` (`bond_rule`) | D-27 decision 3: poison-like step damage that leaves 1 HP; a friendship-like gain per steps |
| Combination (B1) | the Merge Altar (`combine open merge_altar` in the map sandbox) makes one creature out of two party members of level 5 or more: the pair's first types meet in a small table (Fire + Water: Wind, Fire + Earth: Lightning, Water + Earth: Bio, Wind + Water: Light, Light + Darkness: Cosmic; either order) and the result is the first base species of that type, at the pair's average level; it learns its species' combine moves and up to two of the inputs' moves, takes three of their genes at random and the first input's bond (the global `merge_rule`); both inputs are consumed once it is placed | `structure.json` `combine_methods` (`merge_altar`), `battle.json` species `combine_moves` and `messages` (`merge_unfit`, `merge_refused`, `merge_done`, `merge_no_room`), `effects/field.ceffect` (`merge_rule`) | D-28 decision 1: an immediate two-input combination (a fusion-like altar), consumed inputs, a tiny type table |
| Grove (B2) | the Grove (`grove` in the map sandbox) holds two party members (no fee, no experience); every 128 steps (`grove_check`) two of one kin make an egg 50 times in 100 (kins: paw, wing, fin, glow, leaf; a kin-less creature never), then the egg waits until a script takes it: the first input's base species at level 1 with its species' combine moves, both inputs kept; C9: the map sandbox's HUD names the two held records in slot order (`held A + B`: A's base is the egg) | `structure.json` `nursery`, `combine_methods` (`grove`: kind nursery, delivery egg), `step_effects` (`grove_check`), `species_fields` (`kin`, 3 bits, named values) | D-28 decisions 2 and 3: a two-slot nursery, one combine move per line, no fee |
| Eggs (B2) | an egg is a record with the custom role `egg` (1 bit) set and `hatch_counter` (11 bits) holding the steps left: the species' `hatch_cycles` (3 bits, default 5) x 256 steps; every 256 steps (`egg_cycle`) each egg in the party loses 256 (512 with a Nurture member) and at 0 it hatches at level 1 and the `hatch_egg` script asks for its nickname (C9: the letter menu's first row is the name so far); eggs never battle, recruit, use field actions or items; C9: eggs never lead the encounters, transform, gain the Grove's experience or run a step effect's handlers (blight and `bond_rule` skip them); an egg sent to a game without an egg role is refused there (`transfer_eggs`) | `structure.json` `eggs`, `monster_layout` (`egg`, `hatch_counter`; `held_item` 8 bits keeps the record at 64 B), `step_effects` (`egg_cycle`), `effects/passives.ceffect` (Nurture `on hatch`) | D-28 decision 3: 5 x 256 steps, a halving talent |
| Hidden talent (B2) | a third passive slot `hidden` that is never rolled (`rolled: false`); the Sproutle line names Nurture there | `structure.json` `passive_slots`, `battle.json` species `passives` | the egg-speeding talent stays out of the normal rolls |

## Design notes

- **Levels count, but low levels are not hopeless.** Stats start from a small share of the base (base / 8 for HP,
  base / 16 for the others) and then grow linearly with the level. The damage formula multiplies by
  `level + 5` and pads Defence by 10. At the same level a 60-power hit takes about a third of the target's HP
  (28 % at level 5, 39 % at level 50). A creature 4 levels ahead deals about twice the damage it takes, and one
  10 levels ahead about three times.
- **One roll, at the end.** The crit, same-type, chart and spread factors come first, then the 92..108 % roll on
  the result. A crit uses the target's Defence without its stages (raises and drops alike), so walls are not
  crit-proof and a lowered Defence does not stack with a crit.
- **Stages are linear.** +25 % a stage up to x2 at +4, -15 % a stage down to x0.4 at -4. Accuracy moves by
  10 % a stage of (accuracy minus evasion).
- **Intelligence is a share, not a ratio.** `(2u + 60) / (u + t + 60)` is exactly 1 when both sides are equal.
  It never more than doubles the odds, and it falls toward 0 against a much cleverer target. The odds grid
  (`tests/data/starter/golden/odds_grid.txt`) shows it from Intelligence 10 to 400.
- **EP comes back, MP does not.** The regen phase returns 1 EP plus a sixteenth of the EP missing, so an empty
  pool refills in a few turns while the free Strike carries the battler. MP only comes back from talents
  (Recharge) and charms (Mana Drop).
- **Experience sees the level gap.** `(foe level + 10) / (own level + 10)` gives more for a stronger foe and less
  for a weaker one. A rival's creatures give x1.25. The growth curves are quadratic: about three wins against
  equal foes per level at every level.

## Changing the rules

- Constants: `ruleset.json` `constants`. The compiler checks every expression's range again and refuses one
  that could leave int32.
- Tables and caps: `structure.json` `stages`, `chart.tiers`, `rules` (`level_max`, `stat_cap`, `self_hit`), `growth`,
  `statuses`.
- Temperaments, rivals' AI: `battle.json` `traits`, `ai_profiles`, `parties`.
- Factors in logic: `effects/conditions.ceffect` (Scorch, Shock, Storm, dazed), `effects/passives.ceffect`,
  `effects/items.ceffect`.
- The engine's checks of this sample: `tests/test_starter_sample.py` (the contract) and `tests/starter_golden_tests.cpp`
  (`test_rules()` checks the main numbers directly). A changed rule changes the golden transcripts: re-record them once
  (`CAPSULE_GOLDEN_RECORD=1 ctest -R starter_golden_tests`) and say why. The snapshot needs its reason too
  (`tests/test_generated_snapshot.py`).

## Release downloads and credits

Each CapsuleCreatures release carries this sample, played by `apps/starter_battle`, next to the overworld's map
sandbox (`apps/sandbox_map`), for every platform: the GBA ROMs and the Linux x86_64, Windows x64 and macOS
universal programs (the umbrella's `.github/workflows/release.yml`; each download's README.txt gives the controls
and how to open it). Both ROMs target the smallest D-5 cartridge, 8 MB (`reference/project.json` `rom.cart_mb`;
the build fails past it, `cmake/gba_rom_size.cmake`); the sample's ROM is about 166 KB.

Credits: the Starter's creatures, moves, numbers and shape art are the engine's own, made for this sample. The GBA
build is based in part on the work of devkitPro (https://devkitpro.org): it links devkitPro's libgba and
devkitARM's start-up code. The desktop builds use SDL2 (https://libsdl.org, zlib licence).
