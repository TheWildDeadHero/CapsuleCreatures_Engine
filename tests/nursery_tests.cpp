/// @file
/// Eggs and the nursery (D-28 sections 2 and 3, B2) on tests/data/combine (its nest method: odds 50 for one race, 20
/// else; the result the first input's base stage as an egg of 8-step cycles; experience 2 a step; a fee of 10 + 5 a
/// level; the Warm Stone's `on hatch` doubles a cycle; the Incubator may be used on an egg): eggs (make, count down,
/// hatch, the look and name, the refusal flags), the refusals of the systems that use records (combination inputs,
/// item use, battle setup), the nursery (deposit and its refusals, experience per step, levels gained and their
/// moves, the fee, withdraw, the checks' slot orders and odds, the waiting egg, take), the egg cycle and the checks
/// through a StepScope (the hatch hook, the hatch script with egg_member), the nursery port's statements (deposit,
/// withdraw paying the fee, the queries, the party menu, the nickname prompt) and the save image's Nursery section.
/// C9 (the D-28 review's fixes): two eggs hatching in one cycle, more records held than the method takes, eggs gain
/// no nursery experience and skip the step effects' handlers, an input's held item read by name, eggs never lead or
/// transform, `combine open` drawing only after yes (and the eggs' name in its menu and question), a move source's
/// `from` and the pushed moves (the brood method).
/// The script VM's variables, the overworld runner and the text box are stand-ins defined here.
#include "capsule/battle/battle.h"
#include "capsule/battle/data.h"
#include "capsule/battle/encounter_lead.h"
#include "capsule/battle/step_effects.h"
#include "capsule/combine/combine.h"
#include "capsule/combine/eggs.h"
#include "capsule/combine/nursery.h"
#include "capsule/growth/growth.h"
#include "capsule/inventory/bag.h"
#include "capsule/inventory/use.h"
#include "capsule/overworld/overworld.h"
#include "capsule/save/save.h"
#include "capsule/script/combine_port.h"
#include "capsule/script/menu.h"
#include "capsule/script/nursery_port.h"
#include "capsule/script/port.h"
#include "capsule/script/script.h"
#include "capsule/script/step_effects.h"

#include "battle_ids.h"
#include "combine_script_ids.h"

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

/// Variables the stand-in script VM keeps.
constexpr int kStandInSlots = 32;
/// ScriptId the stand-in runner reports when it started none.
constexpr uint16_t kNoneStarted = 0xFFFF;
/// The stand-in yes / no answer before one is set.
constexpr int8_t kNoAnswer = -1;

int32_t g_vars[kStandInSlots] = {};   ///< The stand-in variables.
uint16_t g_started = kNoneStarted;    ///< The script the stand-in runner started last.
int g_scripts = 0;                    ///< Scripts it started.
bool g_runner_busy = false;           ///< The stand-in runner refuses to start a script.
bool g_busy_after_start = false;      ///< C9: a started script keeps running (the runner stays busy).
int8_t g_yes = kNoAnswer;             ///< The stand-in yes / no answer (1 yes, 0 no).
int16_t g_choice = -1;                ///< The stand-in menu's answer (an option, or -1: waiting).
int g_messages = 0;                   ///< Messages shown.
/// Codes the stand-in text box keeps of the last message.
constexpr int kShownCodes = 128;
text::Code g_shown[kShownCodes] = {};   ///< C9: the last message's codes.
/// C9: the option texts of the last menu the stand-in opened.
text::Code g_options[script::kMaxChoices][script::menu::kOptionCodes] = {};
uint8_t g_option_count = 0;             ///< Their count.

} // namespace

/// @name Stand-ins: the script VM's variables, the overworld runner's script start, the text box
///@{
namespace capsule::script
{
/// save::load() calls it after a load.
void bump_state()
{
}
int32_t var(VarId v)
{
   return g_vars[static_cast<uint16_t>(v) % kStandInSlots];
}
void set_var(VarId v, int32_t value)
{
   // the bank's declared range, as the VM clamps
   const VarInfo& info = kBank.vars[static_cast<uint16_t>(v)];
   g_vars[static_cast<uint16_t>(v) % kStandInSlots] = value < info.min ? info.min : (value > info.max ? info.max
                                                                                                     : value);
}
namespace port
{
void message(const text::Code* codes)
{
   int k = 0;
   for (; codes != nullptr && codes[k] != text::kCodeEnd && k < kShownCodes - 1; ++k)
   {
      g_shown[k] = codes[k];
   }
   g_shown[k] = text::kCodeEnd;
   ++g_messages;
}
bool message_busy()
{
   return false;
}
void close_message()
{
}
bool button()
{
   return true;
}
void yesno_open()
{
}
int8_t yesno_poll()
{
   return g_yes;
}
void choice_open(const text::Code* const* options, uint8_t count, uint8_t cancel)
{
   (void)cancel;
   g_option_count = count < script::kMaxChoices ? count : static_cast<uint8_t>(script::kMaxChoices);
   for (uint8_t k = 0; k < g_option_count; ++k)
   {
      int c = 0;
      for (; options[k][c] != text::kCodeEnd && c < script::menu::kOptionCodes - 1; ++c)
      {
         g_options[k][c] = options[k][c];
      }
      g_options[k][c] = text::kCodeEnd;
   }
}
int16_t choice_poll()
{
   return g_choice;
}
} // namespace port
} // namespace capsule::script

namespace capsule::overworld
{
bool can_run_script()
{
   return !g_runner_busy;
}
bool run_script(uint16_t script, uint8_t self)
{
   (void)self;
   if (g_runner_busy)
   {
      return false;
   }
   g_started = script;
   ++g_scripts;
   g_runner_busy = g_busy_after_start;   // C9: the script may still run when the steps are polled again
   return true;
}
} // namespace capsule::overworld
///@}

namespace
{

using namespace battle_ids;
using namespace script_ids;
using namespace capsule::combine;
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
   } while (0)

/// @name The fixture's numbers (tests/data/combine)
///@{
constexpr uint16_t kCycle = 8;             ///< nest's cycle_steps (= the egg cycle's every).
constexpr uint32_t kPixletCycles = 2;      ///< The hatch cycles field's default.
constexpr uint32_t kPupkinCycles = 3;      ///< Pupkin's own.
constexpr int32_t kPixletLook = 1;         ///< Pixlet's egg look.
constexpr int32_t kSameRace = 50;          ///< nest's odds for one race.
constexpr int32_t kOtherRace = 20;         ///< For two.
constexpr uint32_t kGainPerStep = 2;       ///< The nursery_gain pipeline.
constexpr int32_t kFeeBase = 10;           ///< The nursery_fee pipeline: base ...
constexpr int32_t kFeePerLevel = 5;        ///< ... and per level gained.
constexpr uint32_t kIncubatorSteps = 4;    ///< What the Incubator takes off a counter.
constexpr uint8_t kSlots = 3;              ///< The nursery's slots.
constexpr uint32_t kSteadyExp5 = 360;      ///< Experience of level 5 on the steady curve (8n^2 + 32n).
constexpr uint32_t kCounterMax = 0xFFFF;   ///< The hatch counter role's top.
///@}
/// @name The test's own numbers
///@{
constexpr uint32_t kRecordSeed = 91;       ///< Seed of the stream records are made from.
constexpr uint32_t kTestSeed = 5150;       ///< Seed of the checks' stream.
constexpr uint8_t kLevel = 3;              ///< The members' level.
constexpr uint32_t kWalk = 100;            ///< Steps walked with records in the nursery (200 experience: level 5).
constexpr int kMaxChecks = 64;             ///< Checks tried before a 50% check must have hit.
constexpr int kTextCodes = 128;            ///< Codes of one formatted text.
constexpr int32_t kRich = 500;             ///< Money that covers a fee.
constexpr int32_t kPoor = 3;               ///< Money that does not.
///@}

battle::State g_state;               ///< The State lent to the field contexts.
monster::Party g_party{};            ///< The party.
rng::Stream g_rng{kTestSeed};        ///< The draws.

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

/// Whether a record knows a move.
/// @param rec   the record
/// @param move  the move
/// @return true when a slot holds it
bool knows(const monster::Record& rec, uint16_t move)
{
   for (int k = 0; k < battle::move_slots(); ++k)
   {
      if (monster::get(rec, Role::Moves, k) == move)
      {
         return true;
      }
   }
   return false;
}

/// Sets the party.
/// @param recs   its records
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

/// A record made an egg.
/// @param species  species index
/// @return the egg
monster::Record egg_of(uint16_t species)
{
   monster::Record rec = make(species, 1);
   CHECK(make_egg(rec, kCycle));
   return rec;
}

/// Eggs: the counter from the species field times the cycle, counting down, hatching, the look, name and flags.
void test_eggs()
{
   CHECK(eggs_on());
   monster::Record pix = make(SPECIES_PIXLET, kLevel);
   CHECK(!is_egg(pix) && hatch_left(pix) == 0 && !count_down(pix, 1) && !hatch(pix));
   CHECK(hatch_steps(SPECIES_PIXLET, kCycle) == kPixletCycles * kCycle);
   CHECK(hatch_steps(SPECIES_PUPKIN, kCycle) == kPupkinCycles * kCycle);
   CHECK(make_egg(pix, kCycle) && is_egg(pix) && hatch_left(pix) == kPixletCycles * kCycle);
   set_hatch_left(pix, kCounterMax + 1);
   CHECK(hatch_left(pix) == kCounterMax);   // clamped to the role
   set_hatch_left(pix, kCycle + 2);
   CHECK(!count_down(pix, kCycle) && hatch_left(pix) == 2);
   CHECK(count_down(pix, kCycle) && hatch_left(pix) == 0 && is_egg(pix));   // at 0: it hatches (not past)
   monster::set(pix, Role::CurrentHp, 1);
   CHECK(hatch(pix) && !is_egg(pix) && monster::get(pix, Role::CurrentHp) > 1);   // derived: full resources
   // the look and the name
   CHECK(egg_look(pix) == kPixletLook && egg_look(make(SPECIES_CUBE, 1)) == 0);
   const text::Code* name = egg_name();
   CHECK(name != nullptr && name[0] == 'O' && name[1] == 'r' && name[2] == 'b' && name[3] == text::kCodeEnd);
   CHECK(egg_sprite() != 0);
   // the refusal flags: the fixture allows eggs nowhere but the Incubator
   CHECK(!allowed(EggUse::Battle) && !allowed(EggUse::Recruit) && !action_allows_eggs(0));
   CHECK(refuses(egg_of(SPECIES_PIXLET), EggUse::Battle) && !refuses(make(SPECIES_PIXLET, 1), EggUse::Battle));
}

/// Systems refuse eggs: a combination input is unfit, a Salve cannot target one (the Incubator can), and a battle
/// never sends one in.
void test_refusals()
{
   const monster::Record egg = egg_of(SPECIES_PUPKIN);
   const monster::Record cube = make(SPECIES_CUBE, kLevel);
   const monster::Record* recs[] = {&egg, &cube};
   const Preview p = preview(COMBINE_METHOD_NEST, recs, 2);
   CHECK(p.status == Status::Unfit && p.slot == 0);
   // item use out of battle
   const monster::Record party[] = {egg, cube};
   party_of(party, 2);
   inventory::Bag bag{};
   inventory::clear(bag);
   CHECK(inventory::add(bag, ITEM_SALVE, 1) && inventory::add(bag, ITEM_INCUBATOR, 1));
   {
      inventory::FieldUse u(g_state, g_party, 0);
      CHECK(u.use(bag, ITEM_SALVE, 0) == inventory::UseResult::BadTarget);
   }
   const uint32_t before = hatch_left(g_party.members[0]);
   {
      inventory::FieldUse u(g_state, g_party, 0);
      CHECK(u.use(bag, ITEM_INCUBATOR, 0) == inventory::UseResult::Used);
   }
   CHECK(hatch_left(g_party.members[0]) == before - kIncubatorSteps);
   // battle setup: the egg leads the party but the cube is sent in
   battle::Setup su{};
   su.participants[0] = battle::Participant{nullptr, &g_party, nullptr, battle::Controller::Human, 0, 0, 0};
   su.participants[1] = battle::Participant{&battle::kData.parties[0], nullptr, nullptr, battle::Controller::Ai, 1,
                                            0, 0};
   su.count = 2;
   su.format = 0;
   su.weather = battle::kNone8;
   su.seed = kTestSeed;
   CHECK(battle::start(g_state, su));
   battle::Request rq{};
   for (int k = 0; k < kMaxChecks && !battle::request(g_state, rq); ++k)
   {
      battle::step(g_state);
   }
   bool egg_out = false;
   bool cube_out = false;
   for (uint8_t i = 0; i < g_state.battler_count; ++i)
   {
      const battle::Battler& b = g_state.battlers[i];
      if (b.party == 0 && b.position != battle::kBench)
      {
         egg_out = egg_out || b.party_slot == 0;
         cube_out = cube_out || b.party_slot == 1;
      }
   }
   CHECK(!egg_out && cube_out);
}

/// The nursery: deposits and their refusals, experience per step, the levels gained and their moves, the fee,
/// withdraw and its refusals.
void test_deposit_withdraw()
{
   namespace cn = nursery;
   cn::clear();
   CHECK(cn::exists() && cn::slot_count() == kSlots && cn::count() == 0);
   const monster::Record party[] = {make(SPECIES_CUBE, kLevel), make(SPECIES_PUPKIN, kLevel),
                                    make(SPECIES_IMPLET, kLevel), egg_of(SPECIES_PIXLET)};
   party_of(party, 4);
   uint8_t slot = kSlots;
   CHECK(cn::deposit(g_party, 3, &slot) == cn::Result::Refused && slot == kSlots);   // an egg (nest takes none)
   CHECK(cn::deposit(g_party, 9) == cn::Result::BadSlot);
   CHECK(cn::deposit(g_party, 0, &slot) == cn::Result::Ok && slot == 0 && g_party.count == 3);
   CHECK(species_of(g_party.members[0]) == SPECIES_PUPKIN);   // the others moved up
   CHECK(cn::deposit(g_party, 0, &slot) == cn::Result::Ok && slot == 1);
   CHECK(cn::deposit(g_party, 0) == cn::Result::Refused);    // Implet is the last that is not an egg
   CHECK(cn::count() == 2 && cn::record(0) != nullptr && cn::record(2) == nullptr);
   CHECK(cn::odds() == kSameRace);   // Cube and Pupkin: beasts
   // experience per step: 2 a step; 100 steps take level 3 (168) to 368, level 5
   cn::walked(kWalk);
   CHECK(cn::state().gained[0] == kWalk * kGainPerStep && cn::level_now(0) == 5 && cn::levels_gained(0) == 2);
   CHECK(cn::fee(0) == kFeeBase + kFeePerLevel * 2 && cn::fee(2) == 0);
   // withdraw: level 5 with its experience, Rush (Cube learns it at 4), full hit points
   CHECK(cn::withdraw(g_party, 2) == cn::Result::Empty && cn::withdraw(g_party, kSlots) == cn::Result::BadSlot);
   CHECK(cn::withdraw(g_party, 0) == cn::Result::Ok && g_party.count == 3 && cn::count() == 1);
   const monster::Record& back = g_party.members[2];   // at the party's end
   CHECK(species_of(back) == SPECIES_CUBE && monster::get(back, Role::Level) == 5);
   CHECK(monster::get(back, Role::Experience) >= kSteadyExp5 && knows(back, MOVE_RUSH) && knows(back, MOVE_TAP));
   // a full party takes nothing back
   const monster::Record full[] = {make(SPECIES_CUBE, kLevel), make(SPECIES_CUBE, kLevel), make(SPECIES_CUBE, kLevel)};
   party_of(full, 3);
   CHECK(cn::withdraw(g_party, 1) == cn::Result::NoRoom && cn::count() == 1);
}

/// The checks: odds over the held records, a hit builds the egg (the first input's base stage, its counter, the
/// shared level-up move) and keeps it; no second result while one waits; take puts it into the party.
void test_checks()
{
   namespace cn = nursery;
   cn::clear();
   const monster::Record party[] = {make(SPECIES_CUBE, kLevel), make(SPECIES_IMPLET, kLevel),
                                    make(SPECIES_PUPKIN, kLevel)};
   party_of(party, 3);
   CHECK(cn::deposit(g_party, 1) == cn::Result::Ok);   // Implet: a fiend
   CHECK(!cn::check(g_state, g_party, g_rng) && cn::odds() == 0);   // one record: no check
   CHECK(cn::deposit(g_party, 1) == cn::Result::Ok);   // Pupkin: a beast
   CHECK(cn::odds() == kOtherRace);
   CHECK(cn::withdraw(g_party, 0) == cn::Result::Ok);   // Implet back
   CHECK(cn::deposit(g_party, 0) == cn::Result::Ok);    // Cube into slot 0
   CHECK(cn::odds() == kSameRace && cn::count() == 2);
   bool hit = false;
   for (int k = 0; k < kMaxChecks && !hit; ++k)
   {
      hit = cn::check(g_state, g_party, g_rng);
   }
   CHECK(hit && cn::result_ready() && cn::count() == 2);   // kept inputs
   const monster::Record* egg = cn::result();
   CHECK(egg != nullptr && is_egg(*egg) && species_of(*egg) == SPECIES_CUBE);   // slot 0: Cube (its own base)
   CHECK(hatch_left(*egg) == kPixletCycles * kCycle && monster::get(*egg, Role::Level) == 1);
   CHECK(knows(*egg, MOVE_TAP));   // both inputs know Tap: Cube's shared level-up move
   CHECK(!cn::check(g_state, g_party, g_rng));   // one waits already
   CHECK(cn::take_result(g_party) == cn::Result::Ok && g_party.count == 2 && is_egg(g_party.members[1]));
   CHECK(cn::take_result(g_party) == cn::Result::Empty && !cn::result_ready());
}

/// Picks an entry of the open paged menu (its page through "More...", then its option).
/// @param entry  the entry
/// @return the port's poll() after the pick
int16_t pick_entry(int entry)
{
   constexpr int kPerPage = script::kMaxChoices - 1;   // a page's entries before "More..."
   for (int page = 0; page < entry / kPerPage; ++page)
   {
      g_choice = static_cast<int16_t>(kPerPage);
      CHECK(script::nursery::poll() == script::nursery::kNurseryRunning);
   }
   g_choice = static_cast<int16_t>(entry % kPerPage);
   return script::nursery::poll();
}

/// The nursery port's statements: deposit, the queries, withdraw paying its fee (too poor: nothing changes), take,
/// the party menu and the nickname prompt.
void test_port()
{
   namespace cn = nursery;
   namespace np = script::nursery;
   cn::clear();
   const monster::Record party[] = {make(SPECIES_CUBE, kLevel), make(SPECIES_PUPKIN, kLevel),
                                    make(SPECIES_IMPLET, kLevel)};
   party_of(party, 3);
   np::attach(&g_party);
   CHECK(np::query(script::NurseryOp::Deposit, 0) == 1 && np::query(script::NurseryOp::Deposit, 0) == 1);
   CHECK(np::query(script::NurseryOp::Count, 0) == 2 && np::query(script::NurseryOp::Odds, 0) == kSameRace);
   cn::walked(kWalk);
   CHECK(np::query(script::NurseryOp::Levels, 0) == 2 && np::query(script::NurseryOp::Fee, 0) == kFeeBase + 2 *
         kFeePerLevel);
   int32_t paid = 0;
   CHECK(np::withdraw(0, kPoor, paid) == script::kNurseryPoor && paid == 0 && cn::count() == 2);
   CHECK(np::withdraw(0, kRich, paid) == 1 && paid == kFeeBase + 2 * kFeePerLevel && cn::count() == 1);
   CHECK(np::withdraw(1, -1, paid) == 1 && paid == 0 && cn::count() == 0);   // no money variable: no fee
   CHECK(np::query(script::NurseryOp::Ready, 0) == 0 && np::query(script::NurseryOp::Take, 0) == 0);
   // the party menu: waits, then the member + 1 (Back: 0)
   g_choice = -1;
   np::pick_open();
   CHECK(np::poll() == np::kNurseryRunning);
   g_choice = 1;
   CHECK(np::poll() == 2);
   // the nickname prompt: no keeps the name; yes, then letters A and B, Del, C, then the name row: "AC"
   g_yes = 0;
   np::name_open(0);
   CHECK(np::poll() == np::kNurseryRunning && np::poll() == 0);
   g_yes = 1;
   np::name_open(0);
   CHECK(np::poll() == np::kNurseryRunning);   // the question typed: yes / no opens
   CHECK(np::poll() == np::kNurseryRunning);   // yes: the letter menu
   // C9: its first row is the name so far (choosing it ends the name), then Del, then the letters
   constexpr int kNameRow = 0;
   constexpr int kDel = 1;
   constexpr int kA = 2;
   constexpr int kB = 3;
   constexpr int kC = 4;
   CHECK(g_option_count == script::kMaxChoices && monster::get(g_party.members[0], Role::Nickname, 0) != 'A');
   CHECK(g_options[kNameRow][0] == 'N' && g_options[kNameRow][5] == ' ' && g_options[kNameRow][6] == text::kCodeEnd);
   const int picks[] = {kA, kB, kDel, kC};
   for (const int pick : picks)
   {
      CHECK(pick_entry(pick) == np::kNurseryRunning);
   }
   // the menu is back on its first page: "Name: AC" in view
   CHECK(g_options[kNameRow][6] == 'A' && g_options[kNameRow][7] == 'C' && g_options[kNameRow][8] == text::kCodeEnd);
   CHECK(pick_entry(kNameRow) == 1);
   const monster::Record& named = g_party.members[0];
   CHECK(monster::get(named, Role::Nickname, 0) == 'A' && monster::get(named, Role::Nickname, 1) == 'C');
   CHECK(monster::get(named, Role::Nickname, 2) == text::kCodeEnd);
}

/// The egg cycle and the checks through a StepScope: each cycle takes its 8 steps off the eggs (16 with a Warm Stone
/// held: the hatch hook), an egg at 0 hatches and its script starts with egg_member; the check effect runs the
/// nursery's check; the steps walked count into the nursery's experience.
void test_steps()
{
   namespace se = script::step_effects;
   namespace cn = nursery;
   cn::clear();
   const monster::Record party[] = {egg_of(SPECIES_PIXLET), make(SPECIES_IMPLET, kLevel), make(SPECIES_PUPKIN,
                                                                                               kLevel)};
   party_of(party, 3);
   battle::StepScope scope(g_state, g_party, g_rng);
   battle::StepEvent ev{};
   CHECK(se::hatch_script() == raw(SCRIPT_ORB_HATCH));
   // a cycle without the stone: 8 off (16 -> 8)
   se::walked(kCycle);
   while (battle::step_next(ev))
   {
   }
   CHECK(hatch_left(g_party.members[0]) == kPixletCycles * kCycle - kCycle && g_scripts == 0);
   // Implet holds the Warm Stone: the next cycle takes 16 and the egg hatches; its script starts
   monster::set(g_party.members[1], Role::HeldItem, ITEM_WARM_STONE);
   g_runner_busy = true;
   se::walked(kCycle);
   while (battle::step_next(ev))
   {
   }
   CHECK(!is_egg(g_party.members[0]) && g_scripts == 0 && battle::step_busy());   // the runner was busy: it waits
   g_runner_busy = false;
   while (battle::step_next(ev))
   {
   }
   CHECK(g_scripts == 1 && g_started == raw(SCRIPT_ORB_HATCH) && script::var(VAR_EGG_MEMBER) == 0);
   CHECK(!battle::step_busy());
   // the check effect (every 4 steps) runs the nursery's checks over Implet and Pupkin (20%); the steps count
   CHECK(cn::deposit(g_party, 1) == cn::Result::Ok && cn::deposit(g_party, 1) == cn::Result::Ok);
   constexpr uint32_t kCheckEvery = 4;
   for (int k = 0; k < kMaxChecks && !cn::result_ready(); ++k)
   {
      se::walked(kCheckEvery);
      while (battle::step_next(ev))
      {
      }
   }
   CHECK(cn::result_ready() && species_of(*cn::result()) == SPECIES_IMPLET);   // slot 0 first
   CHECK(cn::state().gained[0] > 0 && cn::state().gained[0] == cn::state().gained[1]);
}

/// The save image's Nursery section: written and read back (the records, levels, experience, the waiting result).
void test_save()
{
   namespace cn = nursery;
   cn::clear();
   const monster::Record party[] = {make(SPECIES_CUBE, kLevel), make(SPECIES_PUPKIN, kLevel),
                                    make(SPECIES_IMPLET, kLevel)};
   party_of(party, 3);
   CHECK(cn::deposit(g_party, 0) == cn::Result::Ok && cn::deposit(g_party, 0) == cn::Result::Ok);
   cn::walked(kWalk);
   cn::state().result = egg_of(SPECIES_PIXLET);
   cn::state().pending = 1;
   const cn::State kept = cn::state();
   CHECK(save::section_size(save::Section::Nursery) == save::kNurseryHeadBytes +
         kSlots * (save::kNurserySlotBytes + monster::kLayout.record_bytes) + monster::kLayout.record_bytes);
   const save::Sources src{nullptr, &g_party, nullptr, &cn::state()};
   CHECK(save::write(src) == save::Status::Ok);
   cn::clear();
   CHECK(save::read(src) == save::Status::Ok);
   const cn::State& got = cn::state();
   CHECK(got.used == kept.used && got.pending == 1 && got.gained[0] == kWalk * kGainPerStep);
   CHECK(got.level[1] == kLevel && species_of(got.records[1]) == SPECIES_PUPKIN && is_egg(got.result));
   CHECK(cn::levels_gained(0) == 2);
}


/// @name C9 (the D-28 review's fixes)
///@{

/// The fixture's bond role.
constexpr monster::RoleId kBondRole = battle_ids::ROLE_BOND;
/// Its initial value (structure.json monster_layout).
constexpr uint32_t kBondStart = 70;

/// Steps a StepScope's queue until it waits or is empty.
void drain_steps()
{
   battle::StepEvent ev{};
   while (battle::step_next(ev))
   {
   }
}

/// Finding 1: two eggs reaching 0 in one cycle each get their own hatch script and egg_member; the second script
/// starts (and egg_member is written) only once the first one ended, however often the steps are polled meanwhile.
void test_two_eggs()
{
   namespace se = script::step_effects;
   nursery::clear();
   monster::Record a = egg_of(SPECIES_PIXLET);
   monster::Record b = egg_of(SPECIES_PIXLET);
   set_hatch_left(a, 1);
   set_hatch_left(b, 1);
   const monster::Record party[] = {a, b, make(SPECIES_IMPLET, kLevel)};
   party_of(party, 3);
   g_scripts = 0;
   g_runner_busy = false;
   g_busy_after_start = true;   // a started hatch script keeps running (its message box)
   {
      battle::StepScope scope(g_state, g_party, g_rng);
      se::walked(kCycle);
      drain_steps();
      drain_steps();   // the game polls again next frame while egg 0's script still runs
      CHECK(!is_egg(g_party.members[0]) && !is_egg(g_party.members[1]));
      CHECK(g_scripts == 1 && script::var(VAR_EGG_MEMBER) == 0 && battle::step_busy());
      g_runner_busy = false;   // egg 0's script ended: egg 1's starts with its own member
      drain_steps();
      CHECK(g_scripts == 2 && script::var(VAR_EGG_MEMBER) == 1);
      g_runner_busy = false;
      drain_steps();
      CHECK(!battle::step_busy());
   }
   g_busy_after_start = false;
   g_runner_busy = false;
}

/// Finding 2: a nursery holding more records than its method takes checks a subset of them (the first one in slot
/// order whose inputs fit); with three slots and a two-input method an egg is still made.
void test_three_held()
{
   namespace cn = nursery;
   cn::clear();
   const monster::Record party[] = {make(SPECIES_IMPLET, kLevel), make(SPECIES_PUPKIN, kLevel),
                                    make(SPECIES_IMPLET, kLevel), make(SPECIES_CUBE, kLevel)};
   party_of(party, 4);
   CHECK(cn::deposit(g_party, 0) == cn::Result::Ok && cn::deposit(g_party, 0) == cn::Result::Ok);
   CHECK(cn::odds() == kOtherRace);
   CHECK(cn::deposit(g_party, 0) == cn::Result::Ok && cn::count() == kSlots);
   CHECK(cn::odds() == kOtherRace);   // slots 0 and 1 (Implet, Pupkin): the first subset that fits
   bool made = false;
   for (int k = 0; k < kMaxChecks && !made; ++k)
   {
      made = cn::check(g_state, g_party, g_rng);
   }
   CHECK(made && cn::result_ready() && species_of(*cn::result()) == SPECIES_IMPLET && cn::count() == kSlots);
}

/// Finding 11: an egg left in the nursery gains no experience (the state is set directly: nest takes no eggs).
void test_walk_eggs()
{
   namespace cn = nursery;
   cn::clear();
   const monster::Record party[] = {make(SPECIES_CUBE, kLevel), make(SPECIES_PUPKIN, kLevel)};
   party_of(party, 2);
   CHECK(cn::deposit(g_party, 0) == cn::Result::Ok);
   cn::state().records[1] = egg_of(SPECIES_PIXLET);
   cn::state().used = static_cast<uint8_t>(cn::state().used | 1u << 1);
   cn::walked(kWalk);
   CHECK(cn::state().gained[0] == kWalk * kGainPerStep && cn::state().gained[1] == 0);
   cn::clear();
}

/// Finding 5: a step effect's handlers skip eggs (its allow_eggs is off): the Glow Moss raises its holder's bond on
/// a nest check run, but not an egg's; the egg cycle still counts the egg down.
void test_egg_steps()
{
   namespace se = script::step_effects;
   nursery::clear();
   CHECK(!step_allows_eggs(STEP_EFFECT_NEST_CHECK) && !step_allows_eggs(STEP_EFFECT_EGG_CYCLE));
   constexpr uint32_t kCheckEvery = 4;   // nest_check's every
   monster::Record egg = egg_of(SPECIES_PIXLET);
   monster::Record cube = make(SPECIES_CUBE, kLevel);
   monster::set(egg, Role::HeldItem, ITEM_GLOW_MOSS);
   monster::set(cube, Role::HeldItem, ITEM_GLOW_MOSS);
   const monster::Record party[] = {egg, cube};
   party_of(party, 2);
   const uint32_t egg_steps = hatch_left(g_party.members[0]);
   {
      battle::StepScope scope(g_state, g_party, g_rng);
      se::walked(kCheckEvery);
      drain_steps();
   }
   CHECK(monster::get(g_party.members[0], kBondRole, 0) == kBondStart);
   CHECK(monster::get(g_party.members[1], kBondRole, 0) == kBondStart + 1);
   CHECK(hatch_left(g_party.members[0]) == egg_steps);   // no egg cycle yet (8 steps)
   {
      battle::StepScope scope(g_state, g_party, g_rng);
      se::walked(kCycle);
      drain_steps();
   }
   CHECK(hatch_left(g_party.members[0]) == egg_steps - kCycle);
}

/// Finding 4: an input's held item reads by name in the `on combine` logic (`input.0.item == "Glow Moss"`: the nest
/// egg inherits the first input's bond).
void test_input_item()
{
   namespace cn = nursery;
   constexpr uint32_t kBond = 7;   // the first input's bond
   cn::clear();
   monster::Record cube = make(SPECIES_CUBE, kLevel);
   monster::set(cube, Role::HeldItem, ITEM_GLOW_MOSS);
   monster::set(cube, kBondRole, 0, kBond);
   const monster::Record party[] = {cube, make(SPECIES_PUPKIN, kLevel), make(SPECIES_IMPLET, kLevel)};
   party_of(party, 3);
   CHECK(cn::deposit(g_party, 0) == cn::Result::Ok && cn::deposit(g_party, 0) == cn::Result::Ok);
   bool made = false;
   for (int k = 0; k < kMaxChecks && !made; ++k)
   {
      made = cn::check(g_state, g_party, g_rng);
   }
   CHECK(made && monster::get(*cn::result(), kBondRole, 0) == kBond);
   // without the Moss the logic inherits nothing
   cn::clear();
   monster::set(cube, Role::HeldItem, 0);
   const monster::Record plain[] = {cube, make(SPECIES_PUPKIN, kLevel), make(SPECIES_IMPLET, kLevel)};
   party_of(plain, 3);
   CHECK(cn::deposit(g_party, 0) == cn::Result::Ok && cn::deposit(g_party, 0) == cn::Result::Ok);
   made = false;
   for (int k = 0; k < kMaxChecks && !made; ++k)
   {
      made = cn::check(g_state, g_party, g_rng);
   }
   CHECK(made && monster::get(*cn::result(), kBondRole, 0) == kBondStart);
   cn::clear();
}

/// Findings 6 and 7: an egg never leads the encounters (the fixture has no lead.allow_eggs) and never transforms
/// (no transformation.allow_eggs).
void test_lead_transform_eggs()
{
   CHECK(!allowed(EggUse::Lead) && !allowed(EggUse::Transform));
   const monster::Record first_egg[] = {egg_of(SPECIES_PIXLET), make(SPECIES_CUBE, kLevel)};
   party_of(first_egg, 2);
   CHECK(battle::lead_of(g_party) == 1 && battle::hook_lead_of(g_party) == 1);
   const monster::Record second_egg[] = {make(SPECIES_CUBE, kLevel), egg_of(SPECIES_PIXLET)};
   party_of(second_egg, 2);
   CHECK(battle::lead_of(g_party) == 0);
   // Pixlet transforms at level 12 (level_up): not as an egg
   constexpr uint8_t kGrown = 12;
   monster::Record pix = make(SPECIES_PIXLET, kGrown);
   monster::Record pix_egg = pix;
   CHECK(make_egg(pix_egg, kCycle));
   const monster::Record pair[] = {pix, pix_egg};
   party_of(pair, 2);
   const growth::Context ctx = growth::make_context(growth::Trigger::LevelUp);
   {
      growth::Change c(g_state, g_party, 0, ctx);
      CHECK(c.stage == growth::Stage::Asking);
   }
   {
      growth::Change c(g_state, g_party, 1, ctx);
      CHECK(c.stage == growth::Stage::None);
   }
}

/// Polls the combine port with a menu answer.
/// @param choice  the menu's answer (-1: none)
/// @return the port's poll()
int16_t combine_poll(int16_t choice)
{
   g_choice = choice;
   return script::combine::poll();
}

/// Whether a text equals an ASCII string.
/// @param codes  the text
/// @param ascii  the string
/// @return true when they match
bool says(const text::Code* codes, const char* ascii)
{
   int k = 0;
   for (; ascii[k] != '\0'; ++k)
   {
      if (codes[k] != static_cast<text::Code>(ascii[k]))
      {
         return false;
      }
   }
   return codes[k] == text::kCodeEnd;
}

/// The party members `combine open` picks: the first entry twice (the menu lists the members not picked yet).
/// @param method  method index
void open_first_two(uint8_t method)
{
   script::combine::open(method);
   CHECK(combine_poll(0) == script::combine::kCombineRunning);
   CHECK(combine_poll(0) == script::combine::kCombineRunning);   // the second pick: the question types
}

/// Answers the open question and runs the statement to its end.
/// @param yes  the answer
/// @return its result
int16_t answer(bool yes)
{
   g_yes = kNoAnswer;
   CHECK(combine_poll(-1) == script::combine::kCombineRunning);   // the question typed: yes / no opens
   g_yes = static_cast<int8_t>(yes ? 1 : 0);
   int16_t r = script::combine::kCombineRunning;
   constexpr int kMaxPolls = 64;   // polls before a statement must have ended
   for (int k = 0; k < kMaxPolls && r == script::combine::kCombineRunning; ++k)
   {
      r = combine_poll(-1);
   }
   g_yes = kNoAnswer;
   return r;
}

/// Finding 9: `combine open` asks from the preview ("?" for a random pick) and draws only after yes: declining
/// leaves the stream as it was, so yes after a no gives what yes would have given at once (no reroll); egg inputs
/// and an egg delivery show the eggs' name in the menu and the question.
void test_combine_open()
{
   constexpr uint8_t kSynthLevel = 10;   // synthesis' requirement
   const monster::Record pair[] = {make(SPECIES_PIXLET, kSynthLevel), make(SPECIES_PUPKIN, kSynthLevel)};
   script::combine::attach(&g_state, &g_party, nullptr, &g_rng);
   script::combine::set_menus(true);
   const rng::Stream start = g_rng;
   // no: nothing changes, the stream included
   party_of(pair, 2);
   open_first_two(COMBINE_METHOD_SYNTHESIS);
   CHECK(says(g_shown, "Pixlet + Pupkin -> ??"));
   CHECK(answer(false) == script::combine::kCombineBack && g_party.count == 2);
   rng::Stream probe = g_rng;
   rng::Stream fresh = start;
   constexpr uint32_t kProbeRange = 1000;   // a draw that tells two streams apart
   CHECK(rng::below(probe, kProbeRange) == rng::below(fresh, kProbeRange));
   // yes: the draw (synthesis consumes its inputs: the result takes the first one's slot)
   open_first_two(COMBINE_METHOD_SYNTHESIS);
   CHECK(answer(true) == script::combine::kCombineDone && g_party.count == 1);
   const uint16_t drawn = species_of(g_party.members[0]);
   CHECK(drawn == SPECIES_PUPKIN || drawn == SPECIES_GRIMMAW);
   // the same stream without the no first: the same result
   g_rng = start;
   party_of(pair, 2);
   open_first_two(COMBINE_METHOD_SYNTHESIS);
   CHECK(answer(true) == script::combine::kCombineDone && species_of(g_party.members[0]) == drawn);
   // an egg input and an egg delivery (brood): the eggs' name, never the species
   const monster::Record brood[] = {egg_of(SPECIES_PIXLET), make(SPECIES_CUBE, kLevel)};
   party_of(brood, 2);
   script::combine::open(COMBINE_METHOD_BROOD);
   CHECK(g_option_count == 2 && says(g_options[0], "Orb") && says(g_options[1], "Cube"));
   CHECK(combine_poll(0) == script::combine::kCombineRunning && combine_poll(0) == script::combine::kCombineRunning);
   CHECK(says(g_shown, "Orb + Cube -> Orb?"));
   CHECK(answer(false) == script::combine::kCombineBack);
   // a nursery method does not run here
   script::combine::open(COMBINE_METHOD_NEST);
   CHECK(combine_poll(-1) == script::combine::kCombineBack);
}

/// Finding 10: a move source's `from` reads one input (its moves in slot order) and `push` learns the candidates
/// after the result's own moves (Spark, Tap), each pushing the first out when full: brood's b knows Rush, Burst and
/// Drain (Burst is Pixlet's combine move); a's own moves are no candidates.
void test_move_from_push()
{
   monster::Record a = make(SPECIES_PIXLET, kLevel);
   monster::Record b = make(SPECIES_CUBE, kLevel);
   const uint16_t b_moves[] = {MOVE_RUSH, MOVE_BURST, MOVE_DRAIN, monster::kNoMove};
   for (int k = 0; k < battle::move_slots(); ++k)
   {
      monster::set(b, Role::Moves, k, b_moves[k]);
      monster::set(a, Role::Moves, k, k == 0 ? MOVE_SMITE : monster::kNoMove);
   }
   const monster::Record pair[] = {a, b};
   party_of(pair, 2);
   const InputRef refs[] = {{From::Party, 0, 0}, {From::Party, 0, 1}};
   Combination c(g_state, g_party, nullptr, COMBINE_METHOD_BROOD, refs, 2, g_rng);
   CHECK(c.stage() == Stage::Ready && c.candidate_count() == 3);
   CHECK(c.candidate(0) == MOVE_BURST && c.candidate(1) == MOVE_RUSH && c.candidate(2) == MOVE_DRAIN);
   const monster::Record& r = c.record();
   CHECK(monster::get(r, Role::Moves, 0) == MOVE_TAP && monster::get(r, Role::Moves, 1) == MOVE_BURST);
   CHECK(monster::get(r, Role::Moves, 2) == MOVE_RUSH && monster::get(r, Role::Moves, 3) == MOVE_DRAIN);
   CHECK(c.place() == Status::Ok && is_egg(g_party.members[2]));
}
///@}

} // namespace

int main()
{
   test_eggs();
   test_refusals();
   test_deposit_withdraw();
   test_checks();
   test_port();
   test_steps();
   test_save();
   test_two_eggs();
   test_three_held();
   test_walk_eggs();
   test_egg_steps();
   test_input_item();
   test_lead_transform_eggs();
   test_combine_open();
   test_move_from_push();
   std::printf("nursery tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
