/// @file
/// Step effects (F3, D-27 section 4) on tests/data/steps: the counters in the script bank's implicit variables
/// (walked(), the due bits), and a StepScope's runs over a party (the steps hook on the global rule, a status, a
/// passive and a held item; the survive and faint rules; the hurt / survived / fainted messages; the role writes;
/// the hatch counter's text; a passive starting a timed field effect; the run's script with the member and outcome
/// variables). The script VM's variables and the overworld runner are stand-ins defined here.
#include "capsule/battle/battle.h"
#include "capsule/battle/data/field.h"
#include "capsule/battle/step_effects.h"
#include "capsule/overworld/overworld.h"
#include "capsule/save/save.h"
#include "capsule/script/field_effects.h"
#include "capsule/script/script.h"
#include "capsule/script/step_effects.h"

#include "battle_ids.h"
#include "steps_script_ids.h"

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
constexpr int kStandInSlots = 64;
/// ScriptId the stand-in runner reports when it started none.
constexpr uint16_t kNoneStarted = 0xFFFF;

int32_t g_vars[kStandInSlots] = {};      ///< The stand-in variables.
int g_bumps = 0;                         ///< C8: script::bump_state() calls (save::load()).
uint16_t g_started = kNoneStarted;       ///< The script the stand-in runner started last.
int g_scripts = 0;                       ///< Scripts it started.
bool g_runner_busy = false;              ///< The stand-in runner refuses to start a script.

} // namespace

/// @name Stand-ins: the script VM's variables, the overworld runner's script start
///@{
namespace capsule::script
{
/// C8: script::bump_state() (save::load() calls it after a load), counted.
void bump_state()
{
   ++g_bumps;
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
   return true;
}
} // namespace capsule::overworld
///@}

namespace
{

using namespace battle_ids;

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

/// @name The test party
///@{
constexpr uint8_t kLevel = 10;         ///< Every member's level.
constexpr uint32_t kSeed = 0x57E9;     ///< Stream seed of the members' random parts.
constexpr uint8_t kStung = 0;          ///< Slot of the stung Blob.
constexpr uint8_t kRotting = 1;        ///< Slot of the rotting Cube (passive Warm).
constexpr uint8_t kLantern = 2;        ///< Slot of the Zap (passive Lantern) holding the Bell.
constexpr uint8_t kPlain = 3;          ///< Slot of the Fern (Calm, nothing held).
constexpr uint8_t kMembers = 4;        ///< Members.
constexpr uint32_t kStungHp = 2;       ///< The Blob's hit points (the sting takes 1 a run).
constexpr uint32_t kRotHp = 5;         ///< The Cube's (the rot takes 3 a run).
constexpr uint32_t kHatch = 2;         ///< The Zap's hatch counter.
constexpr int32_t kRotDamage = 3;      ///< What the rot takes.
constexpr uint32_t kBondWarm = 2;      ///< A warm member's bond gain (global 1, Warm 1).
constexpr uint32_t kBondBell = 3;      ///< A bell holder's (global 1, Bell 2).
constexpr int32_t kGlowAmount = 5;     ///< What the Lantern starts the glow with.
///@}

battle::State g_state;      ///< The State lent to the field contexts.
monster::Party g_party;     ///< The test party.
rng::Stream g_rng{kSeed};   ///< The handlers' stream.

/// Builds the test party.
void make_party()
{
   g_party = monster::Party{};
   rng::Stream r{kSeed};
   battle::make_monster(g_party.members[kStung], SPECIES_BLOB, kLevel, r);
   battle::make_monster(g_party.members[kRotting], SPECIES_CUBE, kLevel, r);
   battle::make_monster(g_party.members[kLantern], SPECIES_ZAP, kLevel, r);
   battle::make_monster(g_party.members[kPlain], SPECIES_FERN, kLevel, r);
   g_party.count = kMembers;
   monster::set(g_party.members[kStung], monster::Role::PrimaryStatus, STATUS_STING);
   monster::set(g_party.members[kStung], monster::Role::CurrentHp, kStungHp);
   monster::set(g_party.members[kRotting], monster::Role::PrimaryStatus, STATUS_ROT);
   monster::set(g_party.members[kRotting], monster::Role::CurrentHp, kRotHp);
   monster::set(g_party.members[kLantern], monster::Role::HeldItem, ITEM_BELL);
   monster::set(g_party.members[kLantern], ROLE_HATCH, 0, kHatch);
}

/// A member's role.
/// @param slot  party slot
/// @param role  role ID
/// @return its value
uint32_t role_of(uint8_t slot, monster::RoleId role)
{
   return monster::get(g_party.members[slot], role);
}

/// Counts the Message events with one message id in the State (and drops every queued event).
/// @param msg  the message id
/// @return how many
int take_messages(uint8_t msg)
{
   int n = 0;
   battle::Event e{};
   while (battle::pop_event(g_state, e))
   {
      if (e.kind == battle::EventKind::Message && e.a == msg)
      {
         ++n;
      }
   }
   return n;
}

/// Counts the Text events (owner texts: the fixture's hatch text is the only one a steps run queues) and drops every
/// queued event.
/// @return how many
int take_texts()
{
   int n = 0;
   battle::Event e{};
   while (battle::pop_event(g_state, e))
   {
      if (e.kind == battle::EventKind::Text)
      {
         ++n;
      }
   }
   return n;
}

/// The counters: the records, walked(), the due bits, take_due() in index order.
void test_counters()
{
   namespace se = script::step_effects;
   CHECK(se::count() == NUM_STEP_EFFECTS);
   CHECK(battle::kFieldRules.step_effect_count == NUM_STEP_EFFECTS);
   const script::StepEffectInfo* sting = se::info(STEP_EFFECT_STING);
   CHECK(sting != nullptr && sting->every == 4 && sting->damage == script::StepDamage::Survive);
   CHECK(sting != nullptr && sting->var == static_cast<uint16_t>(script_ids::VAR_STEP_EFFECT_STING));
   CHECK(se::info(NUM_STEP_EFFECTS) == nullptr);
   se::clear_due();
   se::walked(1);
   CHECK(se::counted(STEP_EFFECT_STING) == 1 && se::steps_left(STEP_EFFECT_STING) == 3);
   CHECK(!se::due(STEP_EFFECT_STING) && !se::due(STEP_EFFECT_ROT));
   se::walked(1);   // 2: the rot (every 2) is due and starts over
   CHECK(se::due(STEP_EFFECT_ROT) && se::counted(STEP_EFFECT_ROT) == 0);
   se::walked(2);   // 4: the sting too, the rot again (one due bit)
   CHECK(se::due(STEP_EFFECT_STING) && se::counted(STEP_EFFECT_STING) == 0);
   CHECK(se::due(STEP_EFFECT_HATCH) && se::counted(STEP_EFFECT_HATCH) == 1);
   uint8_t k = 0;
   CHECK(se::take_due(&k) && k == STEP_EFFECT_STING);
   CHECK(se::take_due(&k) && k == STEP_EFFECT_ROT);
   CHECK(se::take_due(&k) && k == STEP_EFFECT_HATCH);
   CHECK(!se::take_due(&k));
   se::walked(0);
   CHECK(!se::take_due(&k));
   // several runs' worth in one call: due once, the rest counts on (bond every 8: 4 + 13 = 17 = 2 * 8 + 1)
   se::walked(13);
   CHECK(se::due(STEP_EFFECT_BOND) && se::counted(STEP_EFFECT_BOND) == 1);
   se::clear_due();
   CHECK(!se::due(STEP_EFFECT_BOND));
}

/// Makes one effect due the way a walk does: every counter at 0, the effect's one step short of its `every`, then
/// one step (the fixture's other effects count to every 2 or more: they are not due).
/// @param effect  the effect
void make_due(uint8_t effect)
{
   namespace se = script::step_effects;
   for (uint8_t k = 0; k < se::count(); ++k)
   {
      script::set_var(script::VarId{se::info(k)->var}, 0);
   }
   se::clear_due();
   script::set_var(script::VarId{se::info(effect)->var}, se::info(effect)->every - 1);
   se::walked(1);
}

/// Runs everything due; counts the members whose handlers ran.
/// @param[out] last  the last event
/// @return the members
int run_all(battle::StepEvent& last)
{
   int n = 0;
   battle::StepEvent e{};
   while (battle::step_next(e))
   {
      last = e;
      ++n;
   }
   return n;
}

/// The sting: survive at 1 (then cured), its message; the members without a handler are passed over.
void test_sting()
{
   make_party();
   battle::StepScope scope(g_state, g_party, g_rng);
   make_due(STEP_EFFECT_STING);
   CHECK(battle::step_busy());
   battle::StepEvent e{};
   // the first run: the Blob loses 1 (2 -> 1): hurt, no hurt message for the sting
   CHECK(battle::step_next(e));
   CHECK(e.effect == STEP_EFFECT_STING && e.member == kStung && e.outcome == battle::StepOutcome::Hurt);
   CHECK(e.hp_before == kStungHp && e.hp_after == kStungHp - 1);
   // the global rule's handler runs for every member too (it does nothing for the sting)
   CHECK(e.handlers == 2);
   const uint8_t survived = script::step_effects::info(STEP_EFFECT_STING)->messages[1];
   CHECK(take_messages(survived) == 0);
   int rest = 0;
   while (battle::step_next(e))
   {
      ++rest;
      CHECK(e.outcome == battle::StepOutcome::None);
   }
   CHECK(rest == kMembers - 1);
   CHECK(!battle::step_busy());
   CHECK(role_of(kStung, monster::id_of(monster::Role::CurrentHp)) == kStungHp - 1);
   // the second run: 1 -> 0, the survive rule gives 1 back, the status is cured, the survived message is queued
   make_due(STEP_EFFECT_STING);
   CHECK(battle::step_next(e));
   CHECK(e.outcome == battle::StepOutcome::Survived && e.hp_before == 1 && e.hp_after == 1);
   CHECK(take_messages(survived) == 1);
   CHECK(run_all(e) == kMembers - 1);
   CHECK(role_of(kStung, monster::id_of(monster::Role::CurrentHp)) == 1);
   CHECK(role_of(kStung, monster::id_of(monster::Role::PrimaryStatus)) == 0);
   CHECK(battle::last_step().member == kPlain);
   CHECK(g_scripts == 0);   // the sting names no script
}

/// The rot: hurt messages, then the faint rule (0, the status stays), the fainted message and the script with the
/// member and outcome variables; a busy runner makes the run wait.
void test_rot()
{
   make_party();
   battle::StepScope scope(g_state, g_party, g_rng);
   const script::StepEffectInfo& rot = *script::step_effects::info(STEP_EFFECT_ROT);
   g_scripts = 0;
   make_due(STEP_EFFECT_ROT);
   battle::StepEvent e{};
   // Blob: no rot (passed over? it has the global rule's handler: it runs, nothing happens)
   CHECK(battle::step_next(e) && e.member == kStung && e.outcome == battle::StepOutcome::None);
   CHECK(battle::step_next(e) && e.member == kRotting && e.outcome == battle::StepOutcome::Hurt);
   CHECK(static_cast<int32_t>(e.hp_after) == static_cast<int32_t>(kRotHp) - kRotDamage);
   CHECK(take_messages(rot.messages[0]) == 1);
   CHECK(run_all(e) == 2);
   CHECK(g_scripts == 1 && g_started == static_cast<uint16_t>(script_ids::SCRIPT_ROT_DOWN));
   CHECK(script::var(script_ids::VAR_STEP_EFFECT_MEMBER) == kRotting);
   CHECK(script::var(script_ids::VAR_STEP_EFFECT_OUTCOME) == static_cast<int32_t>(battle::StepOutcome::Hurt));
   // the next run takes it to 0: fainted, the rot stays; the runner is busy: the run waits for it
   make_due(STEP_EFFECT_ROT);
   g_runner_busy = true;
   CHECK(battle::step_next(e) && e.member == kStung);
   CHECK(battle::step_next(e) && e.member == kRotting && e.outcome == battle::StepOutcome::Fainted);
   CHECK(e.hp_after == 0);
   CHECK(take_messages(rot.messages[2]) == 1);
   CHECK(run_all(e) == 2);
   CHECK(battle::step_busy() && g_scripts == 1);
   g_runner_busy = false;
   CHECK(!battle::step_next(e));
   CHECK(!battle::step_busy() && g_scripts == 2);
   CHECK(script::var(script_ids::VAR_STEP_EFFECT_OUTCOME) == static_cast<int32_t>(battle::StepOutcome::Fainted));
   CHECK(role_of(kRotting, monster::id_of(monster::Role::CurrentHp)) == 0);
   CHECK(role_of(kRotting, monster::id_of(monster::Role::PrimaryStatus)) == STATUS_ROT);
   // a fainted member is not hurt again (the rot's handler needs hit points): no outcome, no script
   make_due(STEP_EFFECT_ROT);
   CHECK(run_all(e) == kMembers);
   CHECK(g_scripts == 2);
}

/// The bond: the global rule, a passive and a held item on their members (role writes into the records); the
/// Lantern starts the glow; the hatch counter runs down and its text is queued at 0.
void test_bond_and_hatch()
{
   make_party();
   battle::StepScope scope(g_state, g_party, g_rng);
   script::field_effects::stop(FIELD_EFFECT_GLOW);
   const uint32_t before[kMembers] = {role_of(kStung, ROLE_BOND), role_of(kRotting, ROLE_BOND),
                                      role_of(kLantern, ROLE_BOND), role_of(kPlain, ROLE_BOND)};
   make_due(STEP_EFFECT_BOND);
   battle::StepEvent e{};
   CHECK(run_all(e) == kMembers);
   CHECK(role_of(kStung, ROLE_BOND) == before[kStung] + 1);
   CHECK(role_of(kRotting, ROLE_BOND) == before[kRotting] + kBondWarm);
   CHECK(role_of(kLantern, ROLE_BOND) == before[kLantern] + kBondBell);
   CHECK(role_of(kPlain, ROLE_BOND) == before[kPlain] + 1);
   CHECK(script::field_effects::remaining(FIELD_EFFECT_GLOW) == kGlowAmount);
   // the hatch counter: 2 -> 1 (no text), 1 -> 0 (the text)
   make_due(STEP_EFFECT_HATCH);
   CHECK(battle::step_next(e) && e.member == kStung);
   CHECK(battle::step_next(e) && e.member == kRotting);
   CHECK(battle::step_next(e) && e.member == kLantern);
   CHECK(take_texts() == 0 && role_of(kLantern, ROLE_HATCH) == kHatch - 1);
   CHECK(run_all(e) == 1);
   make_due(STEP_EFFECT_HATCH);
   CHECK(battle::step_next(e) && battle::step_next(e) && battle::step_next(e) && e.member == kLantern);
   CHECK(take_texts() == 1 && role_of(kLantern, ROLE_HATCH) == 0);
   CHECK(run_all(e) == 1);
   // no scope: nothing runs
}

/// C8 (D-27 review): two members with the same worst outcome: the member variable holds the first in party order.
void test_worst_member()
{
   make_party();
   monster::set(g_party.members[kPlain], monster::Role::PrimaryStatus, STATUS_ROT);
   monster::set(g_party.members[kPlain], monster::Role::CurrentHp, kRotHp);
   battle::StepScope scope(g_state, g_party, g_rng);
   g_scripts = 0;
   make_due(STEP_EFFECT_ROT);
   battle::StepEvent e{};
   CHECK(run_all(e) > 0);
   take_messages(0);   // drops the queued events
   CHECK(g_scripts == 1 && script::var(script_ids::VAR_STEP_EFFECT_MEMBER) == kRotting);
   CHECK(script::var(script_ids::VAR_STEP_EFFECT_OUTCOME) == static_cast<int32_t>(battle::StepOutcome::Hurt));
}

/// Without a StepScope nothing runs (the due bits wait).
void test_no_scope()
{
   battle::StepEvent e{};
   make_due(STEP_EFFECT_BOND);
   CHECK(!battle::step_next(e));
   CHECK(script::step_effects::due(STEP_EFFECT_BOND));
   script::step_effects::clear_due();
}

/// C8 (D-27 review): an expiry not reported yet and a due run belong to the game before a load: save::load() and
/// reset_clock() forget them (overworld::reset() calls reset_clock() and step_effects::clear_due() too).
void test_load_forgets()
{
   const save::Sources none{nullptr, nullptr, nullptr, nullptr};
   CHECK(save::write(none) == save::Status::Ok);
   // a ward ending (pending expiry) and a bond run due
   script::field_effects::stop(FIELD_EFFECT_WARD);
   CHECK(script::field_effects::start(FIELD_EFFECT_WARD, 1));
   script::field_effects::walked(1);
   make_due(STEP_EFFECT_BOND);
   CHECK(script::step_effects::due(STEP_EFFECT_BOND));
   CHECK(save::load(none) == save::Status::Ok);
   uint8_t fx = 0;
   CHECK(!script::field_effects::take_expired(&fx));
   CHECK(!script::step_effects::due(STEP_EFFECT_BOND));
   CHECK(g_bumps == 1);
   // reset_clock() alone forgets an expiry too
   CHECK(script::field_effects::start(FIELD_EFFECT_WARD, 1));
   script::field_effects::walked(1);
   script::field_effects::reset_clock();
   CHECK(!script::field_effects::take_expired(&fx));
}

} // namespace

/// Runs the checks.
/// @return 0 when all pass
int main()
{
   test_counters();
   test_sting();
   test_rot();
   test_bond_and_hatch();
   test_no_scope();
   test_worst_member();
   test_load_forgets();
   std::printf("step_effects_tests: %d checks, %d failed\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
