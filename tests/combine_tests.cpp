/// @file
/// Tests for combination (D-28 section 1, B1) against the battle data compiled from tests/data/combine (invented
/// races and families): the tables, previews of every rule kind (a race table with an average-level pick and its
/// fallback, a 3-input recipe in any order, a recipe that needs a full moon from the game's context port, a family
/// table with a random pick, an expression giving the base stage), the refusals (count, a slot's requirement, odds
/// 0, no rule, a nursery method, the `on combine` logic), the Combination scope (the result record, inheritance:
/// picked genes, a copied role, a "+" counter, a role passed with odds; moves from the sources with the player's,
/// the ordered and the random chooser; consumed and kept inputs from the party and the boxes; no room) and the fee.
#include "capsule/battle/battle.h"
#include "capsule/battle/data.h"
#include "capsule/combine/combine.h"
#include "capsule/growth/growth.h"
#include "capsule/storage/storage.h"

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
using namespace capsule::combine;
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

/// @name The fixture's numbers (tests/data/combine)
///@{
constexpr int kMethods = 5;               ///< fusion, synthesis, echo, nest, brood (C9).
constexpr int32_t kNestSameRace = 50;     ///< B2: the nest method's odds for two inputs of one race.
constexpr uint8_t kImpletBase = 5;        ///< Implet's base level (the fusion result's level).
constexpr uint8_t kGloomclawBase = 16;    ///< Gloomclaw's.
constexpr uint8_t kOwlseerBase = 10;      ///< Owlseer's.
constexpr uint8_t kScaledrakeBase = 24;   ///< Scaledrake's.
constexpr int32_t kFeePerLevel = 10;      ///< fusion's fee: result.level * 10 + count.
constexpr uint8_t kSynthLevel = 10;       ///< synthesis' requirement (level >= 10).
constexpr uint8_t kFusionMoves = 2;       ///< fusion's move limit.
constexpr int kMoveSlots = 4;             ///< The layout's move slots.
constexpr uint32_t kGenes = 7;            ///< The generation list's constant genes.
constexpr uint32_t kQuietBond = 5;        ///< A bond below echo's 10 (the logic refuses).
constexpr uint16_t kBeastFamily = 1;      ///< The family field's value beast (its second name).
///@}
/// @name The test's own numbers
///@{
constexpr uint32_t kRecordSeed = 77;      ///< Seed of the stream records are made from.
constexpr uint32_t kCombineSeed = 4040;   ///< Seed of the combinations' stream.
constexpr uint8_t kLow = 3;               ///< A low level.
constexpr uint8_t kLower = 2;             ///< A lower one.
constexpr uint8_t kHigh = 20;             ///< A level whose average passes every fiend's base level + 1.
constexpr uint32_t kGenesA = 1;           ///< Input a's genes (fusion picks).
constexpr uint32_t kGenesB = 2;           ///< Input b's genes.
constexpr uint32_t kPedigreeGenes = 3;    ///< The synthesis pedigree's genes.
constexpr uint32_t kPlusA = 4;            ///< The pedigree's counter.
constexpr uint32_t kPlusB = 8;            ///< The partner's counter.
constexpr uint32_t kEchoBond = 123;       ///< The echo partner's bond (passed with odds 1 in 1).
constexpr int kRandomTries = 16;          ///< Synthesis runs that must give both base-stage beasts.
constexpr int kTextCodes = 128;           ///< Codes of one formatted text.
///@}

battle::State g_state;                                              ///< The lent State.
monster::Party g_party{};                                           ///< The party.
storage::Buffer<STORAGE_BYTES, STORAGE_USED_BYTES> g_box_bytes{};   ///< The boxes' bytes.
storage::Storage g_boxes = storage::view(g_box_bytes);              ///< The storage boxes.
rng::Stream g_rng{kCombineSeed};                                    ///< The combinations' stream.

/// A fresh record.
/// @param species  species index
/// @param level    level
/// @return the record
monster::Record make(uint16_t species, uint8_t level)
{
   monster::Record rec{};
   rng::Stream r{kRecordSeed + species};
   CHECK(battle::make_monster(rec, species, level, r));
   return rec;
}

/// A record's species.
/// @param rec  the record
/// @return its species index
uint16_t species_of(const monster::Record& rec)
{
   return battle::find_species(AssetKey{monster::get(rec, Role::SpeciesKey)});
}

/// Sets every element of a per-stat role.
/// @param rec   the record
/// @param role  the role
/// @param v     the value
void set_all(monster::Record& rec, Role role, uint32_t v)
{
   for (int e = 0; e < monster::count(role); ++e)
   {
      monster::set(rec, role, e, v);
   }
}

/// A preview of records.
/// @param method  method
/// @param a       first input
/// @param b       second input
/// @param c       third input, or nullptr
/// @return the preview
Preview look(uint8_t method, const monster::Record& a, const monster::Record& b, const monster::Record* c = nullptr)
{
   const monster::Record* recs[] = {&a, &b, c};
   return preview(method, recs, static_cast<uint8_t>(c != nullptr ? 3 : 2));
}

/// A party of records.
/// @param recs   the records
/// @param count  how many
void party_of(const monster::Record* recs, uint8_t count)
{
   g_party = monster::Party{};
   for (uint8_t k = 0; k < count; ++k)
   {
      g_party.members[k] = recs[k];
   }
   g_party.count = count;
}

/// How many events with a message the State holds (they are taken).
/// @return the count
int texts()
{
   int n = 0;
   battle::Event e{};
   text::Code out[kTextCodes] = {};
   while (battle::pop_event(g_state, e))
   {
      n += battle::format_text(g_state, e, out, kTextCodes) ? 1 : 0;
   }
   return n;
}

/// The custom role named plus / bond (the fixture's layout).
/// @return its RoleId
monster::RoleId plus_role()
{
   return monster::RoleId{ROLE_PLUS};
}

/// The bond role.
/// @return its RoleId
monster::RoleId bond_role()
{
   return monster::RoleId{ROLE_BOND};
}

/// The tables: four methods, their kinds and counts, the base stages.
void test_tables()
{
   CHECK(method_count() == kMethods && NUM_COMBINE_METHODS == kMethods);
   const MethodData* fusion = method_at(COMBINE_METHOD_FUSION);
   CHECK(fusion != nullptr && fusion->min_inputs == 2 && fusion->max_inputs == 3 && fusion->rule_count == 3);
   CHECK(fusion->afterwards == Afterwards::Consumed && fusion->chooser == Chooser::Player);
   CHECK(fusion->move_limit == kFusionMoves && fusion->source_count == 1);
   const MethodData* nest = method_at(COMBINE_METHOD_NEST);
   CHECK(nest != nullptr && nest->kind == MethodKind::Nursery && nest->delivery == Delivery::Egg);
   CHECK(method_at(kMethods) == nullptr);
   CHECK(kCombine.base[SPECIES_FAELORD] == SPECIES_PIXLET && kCombine.base[SPECIES_TUSKHORN] == SPECIES_PUPKIN);
   CHECK(kCombine.base[SPECIES_GRIMMAW] == SPECIES_GRIMMAW);
}

/// Fusion previews: the race table's pick (average level + 1, the inputs' own species left out), its fallback,
/// the 3-input recipe in any order, the moon recipe through the game's context value, the count rule.
void test_fusion_preview()
{
   const monster::Record pixlet = make(SPECIES_PIXLET, kLow);
   const monster::Record pupkin = make(SPECIES_PUPKIN, kLower);
   const monster::Record implet = make(SPECIES_IMPLET, kLow);
   // sprite x beast -> fiend: the lowest fiend above level (3 + 2) / 2 + 1 = 3: Implet (5)
   Preview p = look(COMBINE_METHOD_FUSION, pixlet, pupkin);
   CHECK(p.status == Status::Ok && p.species == SPECIES_IMPLET && p.level == kImpletBase && p.rule == 2);
   CHECK(p.odds == 1 && p.fee == kImpletBase * kFeePerLevel + 2);
   // the table is symmetric: beast x sprite too
   CHECK(look(COMBINE_METHOD_FUSION, pupkin, pixlet).species == SPECIES_IMPLET);
   // level 20 + 20: no fiend above 21: the fallback takes the last by base level (Gloomclaw)
   p = look(COMBINE_METHOD_FUSION, make(SPECIES_PIXLET, kHigh), make(SPECIES_PUPKIN, kHigh));
   CHECK(p.status == Status::Ok && p.species == SPECIES_GLOOMCLAW && p.level == kGloomclawBase);
   // the special recipe: three inputs in any order
   p = look(COMBINE_METHOD_FUSION, implet, pixlet, &pupkin);
   CHECK(p.status == Status::Ok && p.species == SPECIES_OWLSEER && p.level == kOwlseerBase && p.rule == 0);
   // three inputs that are no recipe: the table rule wants two (count == 2): refused
   const monster::Record wyrm = make(SPECIES_WYRMLING, kLow);
   CHECK(look(COMBINE_METHOD_FUSION, pixlet, pupkin, &wyrm).status == Status::Refused);
   // wyrm x wyrm: only under a full moon (the game's context value; the manifest's default is half)
   CHECK(look(COMBINE_METHOD_FUSION, wyrm, wyrm).status == Status::Refused);
   growth::set_context_value(CONTEXT_MOON_PHASE, CONTEXT_MOON_PHASE_FULL);
   p = look(COMBINE_METHOD_FUSION, wyrm, wyrm);
   CHECK(p.status == Status::Ok && p.species == SPECIES_SCALEDRAKE && p.level == kScaledrakeBase && p.rule == 1);
   growth::set_context_value(CONTEXT_MOON_PHASE, CONTEXT_MOON_PHASE_HALF);
   // the count: one input is too few
   const monster::Record* one[] = {&pixlet};
   CHECK(preview(COMBINE_METHOD_FUSION, one, 1).status == Status::WrongCount);
   CHECK(preview(kMethods, one, 1).status == Status::BadInput);
}

/// Synthesis, echo and nest previews: a slot's requirement, a recipe, a random pick's group, odds 0, the base stage
/// by expression, a nursery method.
void test_other_previews()
{
   const monster::Record young = make(SPECIES_PUPKIN, kLow);
   const monster::Record grown = make(SPECIES_PUPKIN, kSynthLevel);
   Preview p = look(COMBINE_METHOD_SYNTHESIS, young, grown);
   CHECK(p.status == Status::Unfit && p.slot == 0);
   p = look(COMBINE_METHOD_SYNTHESIS, grown, young);
   CHECK(p.status == Status::Unfit && p.slot == 1);
   p = look(COMBINE_METHOD_SYNTHESIS, grown, grown);
   CHECK(p.status == Status::Ok && p.species == SPECIES_TUSKHORN && p.level == 1);
   // slime x beast -> beast: Pupkin or Grimmaw (the base stages): random, so the preview gives the group
   p = look(COMBINE_METHOD_SYNTHESIS, make(SPECIES_PIXLET, kSynthLevel), grown);
   CHECK(p.status == Status::Ok && p.species == kNoSpecies && p.group == kBeastFamily);
   // C9: its fee (result.base_level * 2) is the highest over the candidates: Grimmaw's base level 28, not Pupkin's 2
   constexpr int32_t kSynthFeePerLevel = 2;
   constexpr int32_t kGrimmawBase = 28;
   CHECK(p.fee == kGrimmawBase * kSynthFeePerLevel);
   // echo: two of one species (odds 100), the base stage of the first; else odds 0
   const monster::Record faelord = make(SPECIES_FAELORD, kHigh);
   p = look(COMBINE_METHOD_ECHO, faelord, faelord);
   CHECK(p.status == Status::Ok && p.species == SPECIES_PIXLET && p.odds == 100 && p.level == 1);
   p = look(COMBINE_METHOD_ECHO, faelord, grown);
   CHECK(p.status == Status::Incompatible && p.odds == 0);
   // B2: a nursery method previews as any other (same race: odds 50; the result is the first input's base stage)
   p = look(COMBINE_METHOD_NEST, faelord, faelord);
   CHECK(p.status == Status::Ok && p.odds == kNestSameRace && p.species == SPECIES_PIXLET);
}

/// A fusion of party members: the player picks two of three candidates, the result takes the first input's slot,
/// the second leaves; inherit_pick gave two genes from the inputs; the fee; the texts.
void test_fusion_combination()
{
   monster::Record a = make(SPECIES_PIXLET, kLow);
   monster::Record b = make(SPECIES_PUPKIN, kLower);
   set_all(a, Role::StatGenes, kGenesA);
   set_all(b, Role::StatGenes, kGenesB);
   const monster::Record cube = make(SPECIES_CUBE, kLow);
   const monster::Record recs[] = {a, b, cube};
   party_of(recs, 3);
   const InputRef refs[] = {{From::Party, 0, 0}, {From::Party, 0, 1}};
   Combination c(g_state, g_party, &g_boxes, COMBINE_METHOD_FUSION, refs, 2, g_rng);
   CHECK(c.stage() == Stage::Choosing && c.result().species == SPECIES_IMPLET);
   CHECK(c.result().fee == kImpletBase * kFeePerLevel + 2);
   // the candidates: Pixlet's Spark and Tap, then Pupkin's Rush (Tap once)
   CHECK(c.candidate_count() == 3 && c.candidate(0) == MOVE_SPARK && c.candidate(1) == MOVE_TAP);
   CHECK(c.candidate(2) == MOVE_RUSH && c.candidate(3) == monster::kNoMove && c.move_limit() == kFusionMoves);
   CHECK(c.choose(2) && !c.choose(2) && c.stage() == Stage::Choosing);
   CHECK(c.choose(0) && c.stage() == Stage::Ready && !c.choose(1));
   // genes: two of the five from the inputs (1 or 2), the rest the generation's 7
   int inherited = 0;
   for (int e = 0; e < monster::count(Role::StatGenes); ++e)
   {
      const uint32_t g = monster::get(c.record(), Role::StatGenes, e);
      CHECK(g == kGenesA || g == kGenesB || g == kGenes);
      inherited += g != kGenes ? 1 : 0;
   }
   CHECK(inherited == 2);
   CHECK(c.place() == Status::Ok && c.stage() == Stage::Placed);
   CHECK(c.placed().from == From::Party && c.placed().index == 0);
   CHECK(g_party.count == 2 && species_of(g_party.members[0]) == SPECIES_IMPLET);
   CHECK(species_of(g_party.members[1]) == SPECIES_CUBE);
   // the moves: the picks in pick order, then Implet's own (Drain, Tap)
   const monster::Record& r = g_party.members[0];
   CHECK(monster::get(r, Role::Moves, 0) == MOVE_RUSH && monster::get(r, Role::Moves, 1) == MOVE_SPARK);
   CHECK(monster::get(r, Role::Moves, 2) == MOVE_DRAIN && monster::get(r, Role::Moves, 3) == MOVE_TAP);
   CHECK(monster::get(r, Role::Level) == kImpletBase && monster::get(r, Role::CurrentHp) > 0);
   CHECK(texts() == 1);   // fuse_done
   CHECK(c.place() == Status::Refused);   // placed once only
}

/// A 3-input fusion with a storage input: the recipe's result in the party input's slot, the boxed input released,
/// the third input's bond copied (inherit), the done choosing path.
void test_fusion_from_boxes()
{
   storage::clear(g_boxes);
   monster::Record implet = make(SPECIES_IMPLET, kLow);
   monster::set(implet, bond_role(), 0, kEchoBond);
   const monster::Record recs[] = {make(SPECIES_PIXLET, kLow), make(SPECIES_PUPKIN, kLow)};
   party_of(recs, 2);
   const uint16_t boxed = storage::deposit(g_boxes, implet);
   CHECK(boxed != storage::kNoSlot);
   const InputRef refs[] = {{From::Party, 0, 1}, {From::Party, 0, 0}, {From::Storage, 0, boxed}};
   Combination c(g_state, g_party, &g_boxes, COMBINE_METHOD_FUSION, refs, 3, g_rng);
   CHECK(c.result().species == SPECIES_OWLSEER && c.stage() == Stage::Choosing);
   c.done_choosing();   // none picked: Owlseer's own moves
   CHECK(c.stage() == Stage::Ready && monster::get(c.record(), bond_role(), 0) == kEchoBond);
   CHECK(c.place() == Status::Ok && c.placed().from == From::Party && c.placed().index == 0);
   CHECK(g_party.count == 1 && species_of(g_party.members[0]) == SPECIES_OWLSEER);
   CHECK(!storage::used(g_boxes, boxed) && storage::count(g_boxes) == 0);
   CHECK(monster::get(g_party.members[0], Role::Moves, 0) == MOVE_REFRESH);
   texts();
}

/// Synthesis: the "+" counter and the pedigree's genes, ordered moves from both inputs and the combine moves (no
/// fill), result level 1; the random pick gives both base-stage beasts over some runs; an unfit input refuses.
void test_synthesis()
{
   monster::Record pedigree = make(SPECIES_PUPKIN, kSynthLevel);
   monster::Record partner = make(SPECIES_PUPKIN, kSynthLevel);
   monster::set(pedigree, plus_role(), 0, kPlusA);
   monster::set(partner, plus_role(), 0, kPlusB);
   set_all(pedigree, Role::StatGenes, kPedigreeGenes);
   const monster::Record recs[] = {pedigree, partner};
   party_of(recs, 2);
   const InputRef refs[] = {{From::Party, 0, 0}, {From::Party, 0, 1}};
   {
      Combination c(g_state, g_party, &g_boxes, COMBINE_METHOD_SYNTHESIS, refs, 2, g_rng);
      CHECK(c.stage() == Stage::Ready && c.result().species == SPECIES_TUSKHORN);
      const monster::Record& r = c.record();
      CHECK(monster::get(r, Role::Level) == 1 && monster::get(r, plus_role(), 0) == (kPlusA + kPlusB) / 4 + 1);
      for (int e = 0; e < monster::count(Role::StatGenes); ++e)
      {
         CHECK(monster::get(r, Role::StatGenes, e) == kPedigreeGenes);
      }
      // known (Rush, Tap), then Tuskhorn's whole combine list (Smite, Refresh)
      CHECK(monster::get(r, Role::Moves, 0) == MOVE_RUSH && monster::get(r, Role::Moves, 1) == MOVE_TAP);
      CHECK(monster::get(r, Role::Moves, 2) == MOVE_SMITE && monster::get(r, Role::Moves, 3) == MOVE_REFRESH);
      CHECK(c.place() == Status::Ok && g_party.count == 1);
      texts();
   }
   // the random pick: Pupkin and Grimmaw both come up
   bool seen_pupkin = false;
   bool seen_grimmaw = false;
   for (int k = 0; k < kRandomTries; ++k)
   {
      const monster::Record pair[] = {make(SPECIES_PIXLET, kSynthLevel), make(SPECIES_PUPKIN, kSynthLevel)};
      party_of(pair, 2);
      Combination c(g_state, g_party, &g_boxes, COMBINE_METHOD_SYNTHESIS, refs, 2, g_rng);
      seen_pupkin = seen_pupkin || c.result().species == SPECIES_PUPKIN;
      seen_grimmaw = seen_grimmaw || c.result().species == SPECIES_GRIMMAW;
      CHECK(c.result().species == SPECIES_PUPKIN || c.result().species == SPECIES_GRIMMAW);
   }
   CHECK(seen_pupkin && seen_grimmaw);
   texts();
   // unfit: refused with the method's message, nothing changes
   const monster::Record weak[] = {make(SPECIES_PUPKIN, kLow), make(SPECIES_PUPKIN, kSynthLevel)};
   party_of(weak, 2);
   Combination c(g_state, g_party, &g_boxes, COMBINE_METHOD_SYNTHESIS, refs, 2, g_rng);
   CHECK(c.stage() == Stage::Refused && c.result().status == Status::Unfit && c.result().slot == 0);
   CHECK(texts() == 1 && c.place() == Status::Refused && g_party.count == 2);
}

/// Echo (kept inputs): the base stage at level 1 joins the party's end, both inputs stay; shared level-up moves;
/// a role passed with odds 1 in 1; the logic's say; the logic refuses a quiet input; odds 0 refuse; no room.
void test_echo()
{
   monster::Record a = make(SPECIES_FAELORD, kHigh);
   monster::Record b = make(SPECIES_FAELORD, kHigh);
   monster::set(b, bond_role(), 0, kEchoBond);
   const monster::Record recs[] = {a, b};
   party_of(recs, 2);
   const InputRef refs[] = {{From::Party, 0, 0}, {From::Party, 0, 1}};
   {
      Combination c(g_state, g_party, nullptr, COMBINE_METHOD_ECHO, refs, 2, g_rng);
      CHECK(c.stage() == Stage::Ready && c.result().species == SPECIES_PIXLET);
      CHECK(monster::get(c.record(), bond_role(), 0) == kEchoBond);
      // Pixlet's learnset moves both inputs know: Spark; then its own fill (Tap)
      CHECK(c.candidate_count() == 1 && c.candidate(0) == MOVE_SPARK);
      CHECK(monster::get(c.record(), Role::Moves, 0) == MOVE_SPARK);
      CHECK(monster::get(c.record(), Role::Moves, 1) == MOVE_TAP);
      CHECK(c.place() == Status::Ok && c.placed().index == 2 && g_party.count == 3);
      CHECK(species_of(g_party.members[0]) == SPECIES_FAELORD && species_of(g_party.members[2]) == SPECIES_PIXLET);
      CHECK(texts() == 2);   // the logic's echo_logic, then echo_done
   }
   // no room: the party is full (party_size 3) and no boxes: nothing changes
   {
      Combination c(g_state, g_party, nullptr, COMBINE_METHOD_ECHO, refs, 2, g_rng);
      CHECK(c.place() == Status::NoRoom && c.stage() == Stage::Ready && g_party.count == 3);
      CHECK(texts() >= 1);
   }
   // with boxes it goes there
   storage::clear(g_boxes);
   {
      Combination c(g_state, g_party, &g_boxes, COMBINE_METHOD_ECHO, refs, 2, g_rng);
      CHECK(c.place() == Status::Ok && c.placed().from == From::Storage && storage::count(g_boxes) == 1);
      texts();
   }
   // the logic refuses a quiet first input
   monster::set(g_party.members[0], bond_role(), 0, kQuietBond);
   {
      Combination c(g_state, g_party, &g_boxes, COMBINE_METHOD_ECHO, refs, 2, g_rng);
      CHECK(c.stage() == Stage::Refused && c.result().status == Status::Refused);
      CHECK(texts() == 1);   // fuse_refused
   }
   // odds 0: two species
   const InputRef apart[] = {{From::Party, 0, 0}, {From::Party, 0, 2}};
   {
      Combination c(g_state, g_party, &g_boxes, COMBINE_METHOD_ECHO, apart, 2, g_rng);
      CHECK(c.stage() == Stage::Refused && c.result().status == Status::Incompatible);
      CHECK(texts() == 1);   // echo_apart
   }
   // bad inputs: one member twice, a member that is not there; a nursery method
   const InputRef twice[] = {{From::Party, 0, 0}, {From::Party, 0, 0}};
   Combination dup(g_state, g_party, &g_boxes, COMBINE_METHOD_ECHO, twice, 2, g_rng);
   CHECK(dup.stage() == Stage::Refused && dup.result().status == Status::BadInput);
   const InputRef gone[] = {{From::Party, 0, 0}, {From::Party, 0, 5}};
   Combination missing(g_state, g_party, &g_boxes, COMBINE_METHOD_ECHO, gone, 2, g_rng);
   CHECK(missing.result().status == Status::BadInput);
   Combination nest(g_state, g_party, &g_boxes, COMBINE_METHOD_NEST, refs, 2, g_rng);
   CHECK(nest.result().status == Status::NotImmediate && g_party.count == 3);
   CHECK(texts() == 0);
}

/// The random chooser fallback (a game without a menu): choose_random() takes the limit from the candidates.
void test_random_chooser()
{
   const monster::Record recs[] = {make(SPECIES_PIXLET, kLow), make(SPECIES_PUPKIN, kLower)};
   party_of(recs, 2);
   const InputRef refs[] = {{From::Party, 0, 0}, {From::Party, 0, 1}};
   Combination c(g_state, g_party, &g_boxes, COMBINE_METHOD_FUSION, refs, 2, g_rng);
   CHECK(c.stage() == Stage::Choosing);
   c.choose_random();
   CHECK(c.stage() == Stage::Ready);
   int picked = 0;
   for (uint8_t k = 0; k < c.candidate_count(); ++k)
   {
      picked += c.chosen(k) ? 1 : 0;
   }
   CHECK(picked == kFusionMoves);
   int known = 0;
   for (int slot = 0; slot < kMoveSlots; ++slot)
   {
      const uint32_t mv = monster::get(c.record(), Role::Moves, slot);
      known += mv == MOVE_SPARK || mv == MOVE_TAP || mv == MOVE_RUSH ? 1 : 0;
   }
   CHECK(known >= kFusionMoves);
   texts();
}

} // namespace

int main()
{
   test_tables();
   test_fusion_preview();
   test_other_previews();
   test_fusion_combination();
   test_fusion_from_boxes();
   test_synthesis();
   test_echo();
   test_random_chooser();
   std::printf("combine tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
