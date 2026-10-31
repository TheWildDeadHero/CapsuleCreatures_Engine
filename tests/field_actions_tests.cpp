/// @file
/// Field actions (F2, D-27 section 3) on tests/data/field: the requirements as growth rules over a member and the
/// game state (knows.MOVE, species fields, passive words, flag.NAME / var.NAME through the script bank's
/// references), the member chosen (the first that qualifies), the dispatcher an ActionScope attaches (a faced
/// behavior, a faced object kind), the menu actions (none, a map tag), the scripts' `field_action available`
/// check, and the flight points' flags. The script VM, the field and the world are stand-ins defined here (their
/// flags and variables in arrays, an attachable dispatcher slot, one map whose tags the test sets).
#include "capsule/battle/data/field.h"
#include "capsule/battle/field_actions.h"
#include "capsule/growth/data.h"
#include "capsule/map/map.h"
#include "capsule/script/field_actions.h"
#include "capsule/script/menu.h"
#include "capsule/script/script.h"

#include "battle_ids.h"
#include "field_script_ids.h"

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

/// Flags and variables the stand-in script VM keeps.
constexpr int kStandInSlots = 64;

bool g_flags[kStandInSlots] = {};      ///< The stand-in flags.
int32_t g_vars[kStandInSlots] = {};    ///< The stand-in variables.
field::ActionRules g_rules = nullptr;  ///< The stand-in field's dispatcher slot.
map::Map g_maps[1] = {};               ///< The stand-in world's one map (its tags change).

} // namespace

/// @name Stand-ins: the script VM's flags and variables, its paged menu, the field's dispatcher slot, the world
///@{
namespace capsule::script
{
bool flag(FlagId f)
{
   return g_flags[static_cast<uint16_t>(f) % kStandInSlots];
}
void set_flag(FlagId f, bool on)
{
   g_flags[static_cast<uint16_t>(f) % kStandInSlots] = on;
}
int32_t var(VarId v)
{
   return g_vars[static_cast<uint16_t>(v) % kStandInSlots];
}
void set_var(VarId v, int32_t value)
{
   g_vars[static_cast<uint16_t>(v) % kStandInSlots] = value;
}
} // namespace capsule::script

namespace capsule::script::menu
{
void open(Paged& m, int total, Fill fill)
{
   m.fill = fill;
   m.total = total;
   m.open = total > 0;
}
int32_t poll(Paged& m)
{
   m.open = false;
   return m.total > 0 ? 0 : kBack;   // the stand-in picks the first entry at once
}
} // namespace capsule::script::menu

namespace capsule::field
{
void set_action_rules(ActionRules rules)
{
   g_rules = rules;
}
ActionRules action_rules()
{
   return g_rules;
}
} // namespace capsule::field

namespace capsule::map
{
const World kWorld = {g_maps, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 1, 0, 0, 0, 0,
                      {0, 0, 0, 0, 0}, nullptr};
} // namespace capsule::map
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
   }                                                                                                                   \
   while (false)

constexpr uint8_t kLevel = 5;                   ///< The members' level.
constexpr uint8_t kWaterBehavior = 1;           ///< tests/data/field/world.json: behavior water.
constexpr uint8_t kNormalBehavior = 0;          ///< Behavior normal.
constexpr uint8_t kBoulderKind = 1;             ///< Object kind boulder (index 0 + 1).
constexpr uint16_t kDarkTag = 1;                ///< Map tag dark (bit 0).
constexpr int32_t kStoryStep = 2;               ///< var.story at which anyone may use the lamp.
constexpr int kMenuRoom = 4;                    ///< Menu actions room.

monster::Party g_party{};   ///< The player's party.

/// A fresh record.
/// @param rec      it
/// @param species  species
void make(monster::Record& rec, uint16_t species)
{
   rng::Stream r{1};
   CHECK(battle::make_monster(rec, species, kLevel, r));
}

/// The party: Cube (only Tap, size 1), Pebble (Spark, size 3), LeadKeen (the Keen passive).
void make_party()
{
   g_party = monster::Party{};
   make(g_party.members[0], SPECIES_CUBE);
   make(g_party.members[1], SPECIES_PEBBLE);
   make(g_party.members[2], SPECIES_LEADKEEN);
   g_party.count = 3;
}

/// The member each requirement picks, with the game state read through the bank's references.
void test_members()
{
   make_party();
   CHECK(battle::kFieldRules.field_action_count == NUM_FIELD_ACTIONS);
   CHECK(script::actions::count() == NUM_FIELD_ACTIONS);
   // paddle: knows Spark and the badge flag
   CHECK(battle::action_member(FIELD_ACTION_PADDLE, g_party) == battle::kBench);
   script::set_flag(script_ids::FLAG_BADGE_ONE, true);
   CHECK(rules::field_state(0) == 1);
   CHECK(battle::action_member(FIELD_ACTION_PADDLE, g_party) == 1);
   // shove: a species field
   CHECK(battle::action_member(FIELD_ACTION_SHOVE, g_party) == 1);
   // lamp: the Keen passive, or anyone from story step 2 on
   CHECK(battle::action_member(FIELD_ACTION_LAMP, g_party) == 2);
   script::set_var(script_ids::VAR_STORY, kStoryStep);
   CHECK(rules::field_state(1) == kStoryStep);
   CHECK(battle::action_member(FIELD_ACTION_LAMP, g_party) == 0);
   script::set_var(script_ids::VAR_STORY, 0);
   // rope: no requirement
   CHECK(battle::action_member(FIELD_ACTION_ROPE, g_party) == 0);
   CHECK(battle::action_member(NUM_FIELD_ACTIONS, g_party) == battle::kBench);
   monster::Party empty{};
   CHECK(battle::action_member(FIELD_ACTION_ROPE, empty) == battle::kBench);
}

/// The dispatcher while an ActionScope lives, and the scripts' check.
void test_dispatch()
{
   make_party();
   CHECK(field::action_rules() == nullptr);
   CHECK(script::actions::available(FIELD_ACTION_ROPE) == script::actions::kNoMember);
   {
      battle::ActionScope scope(g_party);
      CHECK(field::action_rules() != nullptr);
      const field::ActionFace water{script::ActionTarget::Behavior, kWaterBehavior, field::kNoObject, 0, 1, 0};
      // paddle asks: its wrapper runs, with Pebble as the member
      script::set_var(script::VarId{script::kBank.action_member_var}, 0);
      CHECK(field::action_rules()(water) == static_cast<uint16_t>(script_ids::SCRIPT_FIELD_ACTION_ASK_PADDLE));
      CHECK(script::var(script::VarId{script::kBank.action_member_var}) == 1);
      script::set_flag(script_ids::FLAG_BADGE_ONE, false);
      CHECK(field::action_rules()(water) == field::kNoScript);   // nobody may: nothing runs
      script::set_flag(script_ids::FLAG_BADGE_ONE, true);
      const field::ActionFace ground{script::ActionTarget::Behavior, kNormalBehavior, field::kNoObject, 0, 1, 0};
      CHECK(field::action_rules()(ground) == field::kNoScript);
      // shove does not ask: its own script
      const field::ActionFace rock{script::ActionTarget::Object, kBoulderKind, 3, 0, 1, 0};
      CHECK(field::action_rules()(rock) == static_cast<uint16_t>(script_ids::SCRIPT_DO_SHOVE));
      CHECK(script::actions::available(FIELD_ACTION_LAMP) == 2);
      CHECK(script::actions::available(NUM_FIELD_ACTIONS) == script::actions::kNoMember);
   }
   CHECK(field::action_rules() == nullptr);
   CHECK(script::actions::available(FIELD_ACTION_ROPE) == script::actions::kNoMember);
}

/// The menu actions: none-target ones anywhere, tag ones on maps with the tag.
void test_menu()
{
   make_party();
   battle::MenuAction out[kMenuRoom] = {};
   g_maps[0].tags = 0;
   CHECK(battle::menu_actions(g_party, 0, out, kMenuRoom) == 1);
   CHECK(out[0].action == FIELD_ACTION_ROPE && out[0].member == 0);
   g_maps[0].tags = kDarkTag;
   CHECK(battle::menu_actions(g_party, 0, out, kMenuRoom) == 2);
   CHECK(out[0].action == FIELD_ACTION_LAMP && out[0].member == 2);
   CHECK(battle::menu_actions(g_party, 0, out, 1) == 1);
   CHECK(battle::start_action(FIELD_ACTION_LAMP, 2) == static_cast<uint16_t>(script_ids::SCRIPT_FIELD_ACTION_ASK_LAMP));
   CHECK(script::var(script::VarId{script::kBank.action_member_var}) == 2);
   CHECK(battle::start_action(NUM_FIELD_ACTIONS, 0) == script::kNoScriptId);
   g_maps[0].tags = 0;
}

/// The flight point's flag and the menu over the recorded ones.
void test_flight()
{
   CHECK(script::actions::flight_count() == 1);
   CHECK(!script::actions::recorded(script_ids::FLIGHT_POINT_PLAIN));
   script::actions::menu_open();
   CHECK(script::actions::menu_poll() == script::actions::kNoFlight);   // nothing recorded yet
   script::actions::record(script_ids::FLIGHT_POINT_PLAIN);
   CHECK(script::actions::recorded(script_ids::FLIGHT_POINT_PLAIN));
   script::actions::menu_open();
   CHECK(script::actions::menu_poll() == script_ids::FLIGHT_POINT_PLAIN);
   CHECK(script::actions::flight_point(1) == nullptr);
   // no escape variables in this bank: nothing is kept
   AssetKey m{};
   int16_t x = 0;
   int16_t y = 0;
   uint8_t facing = 0;
   script::actions::record_escape(AssetKey{1u}, 1, 2, 0);
   CHECK(!script::actions::escape_point(m, x, y, facing));
}

} // namespace

int main()
{
   test_members();
   test_dispatch();
   test_menu();
   test_flight();
   std::printf("field_actions_tests: %d checks, %d failed\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
