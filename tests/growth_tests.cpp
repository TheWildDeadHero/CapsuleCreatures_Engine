/// @file
/// Tests for growth (D-26 section 4, G4) against the battle data compiled from tests/data/growth: the tables, rule
/// lookup by trigger (level, item, trade with an item used up, a bond with the time of day, a branch on the seed, a
/// stat comparison, the blocked expression), the Change scope (go, stop, move offers, a second creature, form
/// changes by rule), the item use path (`transform` in an item's use_field logic: consumed when it holds, kept when
/// not), forms (base stats, types, a form by item, a battle-only form by a passive), the derived roles that read
/// the species, and training values (gained in battle from the defeated foe's yields within the caps, the `train`
/// primitive, written back; the level_up trigger after the battle).
#include "capsule/battle/battle.h"
#include "capsule/battle/data.h"
#include "capsule/growth/growth.h"
#include "capsule/inventory/bag.h"
#include "capsule/inventory/use.h"

#include "battle_ids.h"

#include <cstdio>
#include <cstdlib>

namespace capsule::hal::system
{
/// Test build: report and abort.
/// @param expr  failed condition
/// @param file  file
/// @param line  line
[[noreturn]] void assert_failed(const char* expr, const char* file, int line)
{
   std::printf("ASSERT %s:%d: %s\n", file, line, expr);
   std::abort();
}
} // namespace capsule::hal::system

namespace
{

using namespace capsule;
using namespace capsule::growth;
using namespace battle_ids;
using monster::Role;

int g_failures = 0;   ///< Failed checks.
int g_checks = 0;     ///< Checks run.

/// Records one check.
/// @param cond  condition that must hold
#define CHECK(cond)                                                                                                    \
   do                                                                                                                  \
   {                                                                                                                   \
      ++g_checks;                                                                                                      \
      if (!(cond))                                                                                                     \
      {                                                                                                                \
         ++g_failures;                                                                                                 \
         std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
      }                                                                                                                \
   }                                                                                                                   \
   while (false)

/// @name The fixture's numbers (tests/data/growth)
///@{
constexpr uint16_t kStatCap = 30;        ///< training.stat_cap.
constexpr uint16_t kTotalCap = 40;       ///< training.total_cap.
constexpr int kTransforms = 12;          ///< Transformation rules in all.
constexpr int kForms = 2;                ///< Shifter's Bright and Storm.
constexpr uint8_t kPipLevel = 5;         ///< Pip becomes Pipster at this level.
constexpr uint8_t kWiggleLevel = 7;      ///< Wiggle's branch level.
constexpr uint8_t kScrapLevel = 8;       ///< Scrap's comparison level.
constexpr uint8_t kHuskLevel = 6;        ///< Husk's level.
constexpr uint32_t kBondNeeded = 200;    ///< Moss's bond.
constexpr uint32_t kPipMark = 2;         ///< Pip's weight / 10 (the derived mark).
constexpr uint32_t kPipsterMark = 6;     ///< Pipster's.
constexpr uint32_t kPipflareMark = 3;    ///< Pipflare's.
constexpr int32_t kBlobVigorGain = 6;    ///< Blob's vigor yield 3, twice.
constexpr int32_t kBlobPowerGain = 2;    ///< Blob's power yield 1, twice.
constexpr uint16_t kBrightPower = 80;    ///< Bright's power base (Shifter's is 40).
constexpr uint16_t kStormPace = 90;      ///< Storm's pace base.
constexpr int32_t kPill = 10;            ///< What a Vigor Pill gives.
constexpr uint32_t kExpBeforeFive = 359; ///< Experience one short of level 5 (8 n^2 + 32 n).
///@}
/// @name The test's own numbers
///@{
constexpr uint32_t kRecordSeed = 4242;   ///< Seed of the stream records are made from.
constexpr uint32_t kBattleSeed = 99;     ///< A battle's seed.
constexpr uint8_t kMember = 0;           ///< The member changes are tried on.
constexpr uint32_t kEvenSeed = 0x00010002u;   ///< A seed whose low half is even.
constexpr uint32_t kOddSeed = 0x00020003u;    ///< One whose low half is odd.
constexpr uint32_t kLowBond = 100;       ///< A bond below Moss's.
constexpr uint32_t kHighBond = 220;      ///< One above it.
constexpr uint8_t kTraining = 20;        ///< A training value that decides Scrap's comparison.
constexpr int kStepLimit = 4000;         ///< Steps a test battle may take.
constexpr uint32_t kSome = 3;            ///< A few of an item.
///@}

battle::State g_state;       ///< The lent State (large; static storage).
monster::Party g_party{};    ///< The party.
inventory::Bag g_bag{};      ///< The bag.

/// A party of one fresh creature.
/// @param species  species index
/// @param level    level
void one(uint16_t species, uint8_t level)
{
   g_party = monster::Party{};
   rng::Stream r{kRecordSeed};
   CHECK(battle::make_monster(g_party.members[kMember], species, level, r));
   g_party.count = 1;
}

/// The member's species.
/// @param slot  party slot
/// @return species index (find_species of its key)
uint16_t species_of(uint8_t slot = kMember)
{
   return battle::find_species(AssetKey{monster::get(g_party.members[slot], Role::SpeciesKey)});
}

/// What a lookup finds for the member.
/// @param t     trigger
/// @param item  the context's item
/// @return the found rule
Found look(Trigger t, uint16_t item = 0)
{
   return find(g_party.members[kMember], make_context(t, item));
}

/// Whether the lent State queued a Transform event with an extra.
/// @param extra  growth::kTransform...
/// @param a      its first value
/// @param b      its second value
/// @return true if it is among the queued events (they are taken)
bool transform_event(uint8_t extra, uint16_t a, uint16_t b)
{
   bool seen = false;
   battle::Event e{};
   while (battle::pop_event(g_state, e))
   {
      seen = seen || (e.kind == battle::EventKind::Transform && e.extra == extra && e.a == a && e.b == b);
   }
   return seen;
}

/// The tables the fixture compiles to.
void test_tables()
{
   CHECK(kGrowth.transform_count == kTransforms && kGrowth.form_count == kForms);
   CHECK(kGrowth.species_count == battle::kData.species_count);
   CHECK(kGrowth.context_count == NUM_CONTEXTS && kGrowth.contexts[CONTEXT_TIME_OF_DAY].source == ContextSource::Clock);
   CHECK(kGrowth.cancel_mask == (1u << TRIGGER_LEVEL_UP | 1u << TRIGGER_AFTER_BATTLE));
   CHECK(kGrowth.form_role == ROLE_FORM && kGrowth.blocked_rule != kNoRule && kGrowth.learn == 1);
   CHECK(kGrowth.training.enabled == 1 && kGrowth.training.stat_cap == kStatCap &&
         kGrowth.training.total_cap == kTotalCap);
   CHECK(kGrowth.training.yield_fields[STAT_MANA] == kNoField);
   CHECK(form_count(SPECIES_SHIFTER) == kForms && form_count(SPECIES_PIP) == 0);
   CHECK(form_of_name(SPECIES_SHIFTER, FORM_STORM) == 2 && form_of_name(SPECIES_PIP, FORM_STORM) == kNoForm);
   CHECK(&species_view(SPECIES_PIP, 1) == &battle::kData.species[SPECIES_PIP]);
   CHECK(species_view(SPECIES_SHIFTER, 1).base[STAT_POWER] == kBrightPower);
   CHECK(species_view(SPECIES_SHIFTER, 1).types[0] == TYPE_GLOW);
   // the context's item is only an item trigger's
   CHECK(make_context(Trigger::LevelUp, ITEM_EMBER_STONE).item == 0);
   CHECK(make_context(Trigger::ItemUse, ITEM_EMBER_STONE).item == ITEM_EMBER_STONE);
   CHECK(make_context(Trigger::Script).values[CONTEXT_REGION] == 1);   // the default port: the manifest's default
}

/// Level, item and the triggers they name; the blocked expression.
void test_lookup()
{
   one(SPECIES_PIP, kPipLevel - 1);
   CHECK(look(Trigger::LevelUp).kind == ChangeKind::None);
   monster::set(g_party.members[kMember], Role::Level, kPipLevel);
   const Found f = look(Trigger::LevelUp);
   CHECK(f.kind == ChangeKind::Species && f.into == SPECIES_PIPSTER && f.also == kNoSpecies);
   CHECK(look(Trigger::AfterBattle).kind == ChangeKind::None);   // on: level_up only
   CHECK(look(Trigger::Trade).kind == ChangeKind::None);
   // the item rule comes first, on item_use only
   CHECK(look(Trigger::ItemUse, ITEM_EMBER_STONE).into == SPECIES_PIPFLARE);
   CHECK(look(Trigger::ItemUse, ITEM_SALVE).kind == ChangeKind::None);
   // held: the blocked expression stops every change
   monster::set(g_party.members[kMember], Role::HeldItem, ITEM_STILL_STONE);
   CHECK(look(Trigger::LevelUp).kind == ChangeKind::Blocked);
   CHECK(look(Trigger::ItemUse, ITEM_EMBER_STONE).kind == ChangeKind::Blocked);
}

/// Trade, with an item used up and without.
void test_trade()
{
   one(SPECIES_SEEDLE, kPipLevel);
   CHECK(look(Trigger::LevelUp).kind == ChangeKind::None);
   CHECK(look(Trigger::Trade).into == SPECIES_SEEDWOOD);
   monster::set(g_party.members[kMember], Role::HeldItem, ITEM_LINK_CORD);
   const Found f = look(Trigger::Trade, ITEM_LINK_CORD);
   CHECK(f.into == SPECIES_SEEDLORD && (f.flags & kTakesItem) != 0);
   {
      Change c(g_state, g_party, kMember, make_context(Trigger::Trade, ITEM_LINK_CORD));
      CHECK(c.stage == Stage::Asking && !c.cancellable());   // trade is not in transformation.cancel
      CHECK(!c.stop());
      CHECK(c.go() == Stage::Done);
   }
   CHECK(species_of() == SPECIES_SEEDLORD);
   CHECK(monster::get(g_party.members[kMember], Role::HeldItem) == 0);
}

/// A bond with the time of day (a context value), a branch on the seed, a stat comparison.
void test_conditions()
{
   one(SPECIES_MOSS, kPipLevel);
   monster::set(g_party.members[kMember], ROLE_BOND, 0, kLowBond);
   CHECK(look(Trigger::LevelUp).kind == ChangeKind::None);
   monster::set(g_party.members[kMember], ROLE_BOND, 0, kHighBond);
   Context ctx = make_context(Trigger::LevelUp);
   ctx.values[CONTEXT_TIME_OF_DAY] = CONTEXT_TIME_OF_DAY_NIGHT;
   CHECK(find(g_party.members[kMember], ctx).into == SPECIES_MOSSNIGHT);
   ctx.values[CONTEXT_TIME_OF_DAY] = CONTEXT_TIME_OF_DAY_DAY;
   CHECK(find(g_party.members[kMember], ctx).into == SPECIES_MOSSDAY);
   static_assert(kHighBond >= kBondNeeded && kLowBond < kBondNeeded);

   one(SPECIES_WIGGLE, kWiggleLevel);
   monster::set(g_party.members[kMember], Role::Seed, kEvenSeed);
   CHECK(look(Trigger::LevelUp).into == SPECIES_WIGGLEA);
   monster::set(g_party.members[kMember], Role::Seed, kOddSeed);
   CHECK(look(Trigger::LevelUp).into == SPECIES_WIGGLEB);

   one(SPECIES_SCRAP, kScrapLevel);
   CHECK(look(Trigger::LevelUp).into == SPECIES_SCRAPMID);   // power == stamina
   monster::set(g_party.members[kMember], Role::StatTraining, STAT_POWER, kTraining);
   CHECK(look(Trigger::LevelUp).into == SPECIES_SCRAPPER);
   monster::set(g_party.members[kMember], Role::StatTraining, STAT_POWER, 0);
   monster::set(g_party.members[kMember], Role::StatTraining, STAT_STAMINA, kTraining);
   CHECK(look(Trigger::LevelUp).into == SPECIES_SCRAPGUARD);
}

/// The Change scope: go (stats, the derived mark, events), stop, move offers, the second creature.
void test_change()
{
   one(SPECIES_PIP, kPipLevel);
   uint16_t before[battle::kMaxStats] = {};
   CHECK(battle::record_stats(g_party.members[kMember], before));
   CHECK(monster::get(g_party.members[kMember], ROLE_MARK) == kPipMark);
   const uint32_t hp_before = monster::get(g_party.members[kMember], Role::CurrentHp);
   {
      Change c(g_state, g_party, kMember, make_context(Trigger::LevelUp));
      CHECK(c.stage == Stage::Asking && c.cancellable() && c.from == SPECIES_PIP);
      CHECK(transform_event(kTransformStart, SPECIES_PIP, SPECIES_PIPSTER));
      // Pipster learns Spark and Rush at 5: two free slots take them
      CHECK(c.go() == Stage::Done);
      CHECK(transform_event(kTransformDone, SPECIES_PIP, SPECIES_PIPSTER));
   }
   CHECK(species_of() == SPECIES_PIPSTER);
   CHECK(monster::get(g_party.members[kMember], Role::Moves, 2) == MOVE_SPARK);
   CHECK(monster::get(g_party.members[kMember], Role::Moves, 3) == MOVE_RUSH);
   CHECK(monster::get(g_party.members[kMember], ROLE_MARK) == kPipsterMark);   // reads the species: re-derived
   // the same base stats, the same stats; full HP stays full
   CHECK(monster::get(g_party.members[kMember], Role::CurrentHp) == hp_before);

   // stopped: nothing changes
   one(SPECIES_PIP, kPipLevel);
   {
      Change c(g_state, g_party, kMember, make_context(Trigger::LevelUp));
      CHECK(c.stop() && c.stage == Stage::Stopped);
      CHECK(c.go() == Stage::Stopped);
   }
   CHECK(species_of() == SPECIES_PIP);

   // every slot taken: Spark offered (forget slot 0), then Rush (keep every move)
   one(SPECIES_PIP, kPipLevel);
   const uint16_t full[] = {MOVE_TAP, MOVE_SMITE, MOVE_DRAIN, MOVE_REFRESH};
   for (int k = 0; k < battle::move_slots(); ++k)
   {
      monster::set(g_party.members[kMember], Role::Moves, k, full[k]);
   }
   {
      Change c(g_state, g_party, kMember, make_context(Trigger::LevelUp));
      CHECK(c.go() == Stage::Learning && c.offered_move() == MOVE_SPARK);
      CHECK(c.answer_learn(0) == Stage::Learning && c.offered_move() == MOVE_RUSH);
      CHECK(c.answer_learn(battle::kKeepMoves) == Stage::Done && c.offered_move() == battle::kNone16);
   }
   CHECK(monster::get(g_party.members[kMember], Role::Moves, 0) == MOVE_SPARK);
   CHECK(monster::get(g_party.members[kMember], Role::Moves, 1) == MOVE_SMITE);

   // also: a second creature into the free slot (no held item, its own species), none with a full party
   one(SPECIES_HUSK, kHuskLevel);
   monster::set(g_party.members[kMember], Role::HeldItem, ITEM_LINK_CORD);
   {
      Change c(g_state, g_party, kMember, make_context(Trigger::LevelUp));
      CHECK(c.found.also == SPECIES_HUSKSHELL);
      CHECK(c.go() == Stage::Done);
   }
   CHECK(g_party.count == 2 && species_of(0) == SPECIES_HUSKFLY && species_of(1) == SPECIES_HUSKSHELL);
   CHECK(monster::get(g_party.members[1], Role::HeldItem) == 0);
   CHECK(monster::get(g_party.members[0], Role::HeldItem) == ITEM_LINK_CORD);
   one(SPECIES_HUSK, kHuskLevel);
   g_party.count = monster::kLayout.party_size;
   for (uint8_t k = 1; k < g_party.count; ++k)
   {
      g_party.members[k] = g_party.members[0];
   }
   {
      Change c(g_state, g_party, kMember, make_context(Trigger::LevelUp));
      CHECK(c.go() == Stage::Done);
   }
   CHECK(g_party.count == monster::kLayout.party_size && species_of(0) == SPECIES_HUSKFLY);
}

/// `transform` in an item's use_field logic: consumed when a rule holds, kept when none does.
void test_item_use()
{
   inventory::clear(g_bag);
   CHECK(inventory::add(g_bag, ITEM_EMBER_STONE, kSome));
   one(SPECIES_MOSS, kPipLevel);
   {
      inventory::FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_EMBER_STONE) == inventory::UseResult::NoEffect);
   }
   CHECK(inventory::count(g_bag, ITEM_EMBER_STONE) == kSome && species_of() == SPECIES_MOSS);
   one(SPECIES_PIP, kPipLevel - 1);
   {
      inventory::FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_EMBER_STONE) == inventory::UseResult::Used);
   }
   CHECK(inventory::count(g_bag, ITEM_EMBER_STONE) == kSome - 1 && species_of() == SPECIES_PIPFLARE);
   CHECK(monster::get(g_party.members[kMember], ROLE_MARK) == kPipflareMark);
}

/// Forms: by rule (after a battle, by day in a region), by item, in battle (battle only), and their stats.
void test_forms()
{
   one(SPECIES_SHIFTER, kPipLevel);
   CHECK(record_form(g_party.members[kMember]) == kBaseForm);
   Context ctx = make_context(Trigger::AfterBattle);
   ctx.values[CONTEXT_TIME_OF_DAY] = CONTEXT_TIME_OF_DAY_DAY;
   ctx.values[CONTEXT_REGION] = 0;
   CHECK(find(g_party.members[kMember], ctx).kind == ChangeKind::None);
   ctx.values[CONTEXT_REGION] = 1;
   const Found f = find(g_party.members[kMember], ctx);
   CHECK(f.kind == ChangeKind::Form && f.form == 1);
   uint16_t before[battle::kMaxStats] = {};
   CHECK(battle::record_stats(g_party.members[kMember], before));
   {
      Change c(g_state, g_party, kMember, ctx);
      CHECK(c.stage == Stage::Asking && !c.cancellable());
      CHECK(c.go() == Stage::Done);
      CHECK(transform_event(kTransformForm, 1, kBaseForm));
   }
   uint16_t after[battle::kMaxStats] = {};
   CHECK(record_form(g_party.members[kMember]) == 1 && battle::record_stats(g_party.members[kMember], after));
   CHECK(after[STAT_POWER] > before[STAT_POWER] && after[STAT_VIGOR] == before[STAT_VIGOR]);
   // the record is Bright already: no rule fires again (by day), the mirror has no effect
   CHECK(find(g_party.members[kMember], ctx).kind == ChangeKind::None);
   // by night its forms[] "base" rule brings it back
   Context night = ctx;
   night.values[CONTEXT_TIME_OF_DAY] = CONTEXT_TIME_OF_DAY_NIGHT;
   const Found back = find(g_party.members[kMember], night);
   CHECK(back.kind == ChangeKind::Form && back.form == kBaseForm);
   inventory::clear(g_bag);
   CHECK(inventory::add(g_bag, ITEM_BRIGHT_MIRROR, 1));
   {
      inventory::FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_BRIGHT_MIRROR) == inventory::UseResult::NoEffect);
   }
   one(SPECIES_SHIFTER, kPipLevel);
   {
      inventory::FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_BRIGHT_MIRROR) == inventory::UseResult::Used);
   }
   CHECK(record_form(g_party.members[kMember]) == 1 && inventory::count(g_bag, ITEM_BRIGHT_MIRROR) == 1);
}

/// Runs a battle of the party against a data party to its end, the player always using its first move slot that
/// it can use.
/// @param foe  the AI's party
/// @return the outcome
battle::Outcome fight(uint16_t foe)
{
   using namespace battle;
   Setup su{};
   su.format = FORMAT_SINGLE;
   su.weather = kNone8;
   su.seed = kBattleSeed;
   su.participants[su.count++] = Participant{nullptr, &g_party, nullptr, Controller::Human, 0, 0, 0};
   su.participants[su.count++] = Participant{&kData.parties[foe], nullptr, nullptr, Controller::Ai, 1, 0, 0};
   CHECK(start(g_state, su));
   Event e{};
   for (int k = 0; k < kStepLimit; ++k)
   {
      while (pop_event(g_state, e))
      {
      }
      const Progress p = step(g_state);
      if (p == Progress::Ended)
      {
         return g_state.outcome;
      }
      Request q{};
      if (p != Progress::Input || !request(g_state, q))
      {
         continue;
      }
      if (q.kind == RequestKind::LearnMove)
      {
         choose(g_state, q.battler, Action{ActionKind::Answer, kKeepMoves, kBench, 0});
         continue;
      }
      // the strongest move it knows: slot 1 (Smite) when it has one, else slot 0
      const uint8_t slot = move_usable(g_state, q.battler, 1) ? 1 : 0;
      choose(g_state, q.battler, Action{ActionKind::Move, slot, kBench, 0});
   }
   return battle::Outcome::None;
}

/// Training gained in battle from the defeated foe's yields, written back; the level_up trigger after the battle.
void test_training_battle()
{
   one(SPECIES_PIP, kPipLevel - 1);
   monster::set(g_party.members[kMember], Role::Experience, kExpBeforeFive);
   CHECK(fight(PARTY_FODDER) == battle::Outcome::Victory);
   const monster::Record& rec = g_party.members[kMember];
   CHECK(monster::get(rec, Role::StatTraining, STAT_VIGOR) == static_cast<uint32_t>(kBlobVigorGain));
   CHECK(monster::get(rec, Role::StatTraining, STAT_POWER) == static_cast<uint32_t>(kBlobPowerGain));
   CHECK(monster::get(rec, Role::StatTraining, STAT_MANA) == 0);   // no yield field
   CHECK(monster::get(rec, Role::Level) == kPipLevel);
   // it levelled up: the level_up trigger once the battle is over
   const uint32_t levelled = levelled_members(g_state, g_party);
   CHECK(levelled == 1u << kMember);
   {
      Change c(g_state, g_party, kMember, make_context(Trigger::LevelUp));
      CHECK(c.go() == Stage::Done);
   }
   CHECK(species_of() == SPECIES_PIPSTER);
   CHECK(monster::get(g_party.members[kMember], Role::StatTraining, STAT_VIGOR) ==
         static_cast<uint32_t>(kBlobVigorGain));   // the Change keeps it
   // C6 (review4 finding 19): a held Heavy Band doubles each gain (the structure's training point)
   CHECK(kGrowth.training.gain_point < battle::kData.point_count);
   one(SPECIES_PIP, kPipLevel - 1);
   monster::set(g_party.members[kMember], Role::HeldItem, ITEM_HEAVY_BAND);
   CHECK(fight(PARTY_FODDER) == battle::Outcome::Victory);
   CHECK(monster::get(g_party.members[kMember], Role::StatTraining, STAT_VIGOR) ==
         static_cast<uint32_t>(2 * kBlobVigorGain));
   CHECK(monster::get(g_party.members[kMember], Role::StatTraining, STAT_POWER) ==
         static_cast<uint32_t>(2 * kBlobPowerGain));
}

/// The caps (add_training(), gain_training()) and the `train` primitive (Vigor Pill).
void test_training_caps()
{
   uint8_t t[battle::kMaxStats] = {};
   CHECK(add_training(t, STAT_VIGOR, kStatCap + kPill) == kStatCap && t[STAT_VIGOR] == kStatCap);
   CHECK(add_training(t, STAT_POWER, kStatCap) == kTotalCap - kStatCap);   // the total cap
   CHECK(add_training(t, STAT_PACE, 1) == 0);
   CHECK(!gain_training(t, SPECIES_BLOB, kPipLevel));                       // full: nothing more
   CHECK(add_training(t, STAT_POWER, -kStatCap) == -(kTotalCap - kStatCap) && t[STAT_POWER] == 0);
   CHECK(gain_training(t, SPECIES_BLOB, kPipLevel) && t[STAT_POWER] == kBlobPowerGain);
   CHECK(!gain_training(t, SPECIES_PIPFLARE, kPipLevel));                   // no yields

   one(SPECIES_PIP, kPipLevel);
   inventory::clear(g_bag);
   CHECK(inventory::add(g_bag, ITEM_VIGOR_PILL, kSome));
   for (int k = 0; k < 2; ++k)
   {
      inventory::FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_VIGOR_PILL) == inventory::UseResult::Used);
   }
   {
      inventory::FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_VIGOR_PILL) == inventory::UseResult::NoEffect);   // the pill's own limit (20)
   }
   CHECK(monster::get(g_party.members[kMember], Role::StatTraining, STAT_VIGOR) == 2u * kPill);
   CHECK(inventory::count(g_bag, ITEM_VIGOR_PILL) == 1);
}

/// A form a passive gives in battle: its types and stats in battle, the record keeps its own.
void test_battle_form()
{
   one(SPECIES_SHIFTER, kPipLevel);
   battle::Setup su{};
   su.format = FORMAT_SINGLE;
   su.weather = battle::kNone8;
   su.seed = kBattleSeed;
   su.participants[su.count++] = battle::Participant{nullptr, &g_party, nullptr, battle::Controller::Human, 0, 0, 0};
   su.participants[su.count++] =
      battle::Participant{&battle::kData.parties[PARTY_FODDER], nullptr, nullptr, battle::Controller::Ai, 1, 0, 0};
   CHECK(battle::start(g_state, su));
   bool formed = false;
   battle::Event e{};
   for (int k = 0; k < kStepLimit && !formed; ++k)
   {
      battle::step(g_state);
      while (battle::pop_event(g_state, e))
      {
         formed = formed || (e.kind == battle::EventKind::Transform && e.extra == kTransformForm);
      }
   }
   const battle::Battler& b = g_state.battlers[0];
   CHECK(formed && b.form == form_of_name(SPECIES_SHIFTER, FORM_STORM) && b.types[0] == TYPE_GLOW);
   CHECK(b.stats[STAT_PACE] > 0 && species_view(b.species, b.form).base[STAT_PACE] == kStormPace);
   CHECK(fight(PARTY_FODDER) == battle::Outcome::Victory);
   CHECK(record_form(g_party.members[kMember]) == kBaseForm);   // battle only
}

} // namespace

int main()
{
   test_tables();
   test_lookup();
   test_trade();
   test_conditions();
   test_change();
   test_item_use();
   test_forms();
   test_training_battle();
   test_training_caps();
   test_battle_form();
   std::printf("growth_tests: %d checks, %d failed\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
