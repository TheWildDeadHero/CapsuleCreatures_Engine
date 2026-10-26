/// @file
/// Tests for the battle's Item action, recruitment and the storage boxes (D-26 section 3, G3) against the battle
/// data compiled from tests/data/recruit: Salve used from the bag on a member (consumed only when it had an effect),
/// an item the bag lacks refused; Lure (bonus 10) and Snare (bonus 255: sure) recruit an unowned creature through 3
/// checks against the fixture's recruit_odds (the rate scaled by the missing HP, the item and status bonuses in
/// tenths, n in 256), an event per check, Outcome::Recruited, the record (hit points and status kept, the item in
/// the orb_used role, the player as its owner) in the party, else in the storage boxes (2 x 3: Pen, Barn); refusals
/// (an owned party, SETUP_NO_RECRUIT, no room) keep the item; the Keeper party's careful profile uses its Salve
/// below half HP and its Beacon through the item's own ai_score; the storage module and the save image's Boxes
/// section.
#include "capsule/battle/battle.h"
#include "capsule/hal/save.h"
#include "capsule/inventory/bag.h"
#include "capsule/save/save.h"
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
using namespace capsule::battle;
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

/// @name The fixture's numbers (tests/data/recruit)
///@{
constexpr int kChecks = 3;                 ///< recruit.checks.
constexpr int32_t kSalveHp = 10;           ///< What Salve restores.
constexpr uint32_t kOutOf = 256;           ///< RECRUIT_OUT_OF.
constexpr int32_t kBlobRate = 200;         ///< Blob's recruit_rate.
constexpr int32_t kTenths = 10;            ///< RECRUIT_TENTHS (bonuses are tenths).
constexpr int32_t kLureBonus = 10;         ///< Lure's bonus.
constexpr int32_t kSureBonus = 255;        ///< Snare's bonus (RECRUIT_SURE_BONUS).
constexpr int32_t kNoStatusBonus = 10;     ///< status_bonus none.
constexpr int32_t kSleepyBonus = 20;       ///< status_bonus Sleepy.
constexpr uint8_t kSleepy = 1;             ///< Status id of Sleepy.
constexpr int kBoxes = 2;                  ///< storage.box_count.
constexpr int kBoxSize = 3;                ///< storage.box_size.
constexpr int kPartySize = 3;              ///< monster_layout.party_size.
///@}
/// @name The test's own numbers
///@{
constexpr uint32_t kRecordSeed = 4242;     ///< Seed of the stream records are made from.
constexpr uint8_t kLevel = 20;             ///< The player's creature's level.
constexpr uint8_t kWildLevel = 5;          ///< The unowned creature's level.
constexpr uint32_t kOwner = 0x1234;        ///< The player's owner id.
constexpr uint32_t kSeed = 99;             ///< The battles' seed.
constexpr int32_t kHurt = 15;              ///< Hit points the player's creature lacks.
constexpr uint32_t kSome = 3;              ///< A few of an item.
constexpr int kStepLimit = 2000;           ///< Steps a battle may take in a test.
constexpr int kLures = 9;                  ///< Lures thrown until one recruits (or they run out: Orbs hold 9).
constexpr uint8_t kPlayer = 0;             ///< The player's battler.
constexpr uint8_t kFoe = 1;                ///< The other party's battler.
constexpr uint8_t kFoePosition = 1;        ///< Its position in the Single format.
constexpr uint8_t kLowHp = 1;              ///< An AI battler's hit points below half.
constexpr size_t kImageBytes = 4096;       ///< Room for the fixture's image.
///@}

battle::State g_state;             ///< The battle (large; static storage).
monster::Party g_party{};          ///< The player's party.
monster::Party g_wild{};           ///< The unowned creature.
inventory::Bag g_bag{};            ///< The player's bag.
storage::Buffer<STORAGE_BYTES, STORAGE_USED_BYTES> g_box_bytes{};   ///< The boxes' bytes (C6: the manifest's size).
storage::Storage g_storage = storage::view(g_box_bytes);            ///< The storage boxes.

/// What one battle showed.
struct Seen
{
   int checks;      ///< Recruit Check moments.
   int moments[kRecruitMoments];   ///< Recruit moments by kind.
   int uses;        ///< UseItem events.
   uint8_t user;    ///< The last UseItem's user.
   uint16_t item;   ///< ...and item.
};

Seen g_seen{};   ///< The current battle's events.

/// A fresh creature record.
/// @param rec      the record
/// @param species  species
/// @param level    level
void make(monster::Record& rec, uint16_t species, uint8_t level)
{
   rng::Stream r{kRecordSeed + species};
   CHECK(make_monster(rec, species, level, r));
}

/// The player's party (a hurt Blob) and an empty bag and storage.
void reset()
{
   g_party = monster::Party{};
   make(g_party.members[0], SPECIES_BLOB, kLevel);
   g_party.count = 1;
   g_wild = monster::Party{};
   make(g_wild.members[0], SPECIES_BLOB, kWildLevel);
   g_wild.count = 1;
   inventory::clear(g_bag);
   storage::clear(g_storage);
}

/// A battle against the unowned creature (or a party given as data).
/// @param foe    the party data, or nullptr: the unowned creature
/// @param flags  SetupFlag bits
/// @return true if it started
bool begin(const PartyData* foe, uint8_t flags = 0)
{
   Setup su{};
   su.participants[0] = Participant{nullptr, &g_party, nullptr, Controller::Human, 0, 0, 0};
   su.participants[0].bag = &g_bag;
   su.participants[1] = foe != nullptr ? Participant{foe, nullptr, nullptr, Controller::Ai, 1, 0, 0}
                                       : Participant{nullptr, &g_wild, nullptr, Controller::Ai, 1,
                                                     static_cast<uint8_t>(1u << PARTY_UNOWNED), 0};
   su.count = 2;
   su.format = FORMAT_SINGLE;
   su.flags = flags;
   su.weather = kNone8;
   su.seed = kSeed;
   su.owner_id = kOwner;
   su.storage = &g_storage;
   g_seen = Seen{};
   return start(g_state, su);
}

/// Takes the queued events, counting the G3 ones.
void drain()
{
   Event e;
   while (pop_event(g_state, e))
   {
      if (e.kind == EventKind::Recruit && e.extra < kRecruitMoments)
      {
         ++g_seen.moments[e.extra];
         g_seen.checks += e.extra == static_cast<uint8_t>(RecruitMoment::Check) ? 1 : 0;
      }
      if (e.kind == EventKind::UseItem)
      {
         ++g_seen.uses;
         g_seen.user = e.battler;
         g_seen.item = e.a;
      }
   }
}

/// Runs the battle until it asks the player for a turn's action (or ends).
/// @return true when it waits for the action
bool to_action()
{
   for (int k = 0; k < kStepLimit; ++k)
   {
      drain();
      const Progress p = step(g_state);
      if (p == Progress::Ended)
      {
         drain();
         return false;
      }
      Request r;
      if (p == Progress::Input && request(g_state, r))
      {
         if (r.kind == RequestKind::Action)
         {
            return true;
         }
         // anything else (a replacement): the first one the battle takes
         for (uint8_t i = 0; i < g_state.battler_count; ++i)
         {
            if (choose(g_state, r.battler, Action{ActionKind::Switch, 0, kBench, i}))
            {
               break;
            }
         }
      }
   }
   return false;
}

/// An Item action.
/// @param item     item id
/// @param target   position (battler targets)
/// @param battler  member battler (member targets)
/// @return the action
Action item_action(uint16_t item, uint8_t target, uint8_t battler)
{
   Action a{ActionKind::Item, 0, target, battler};
   a.item = item;
   return a;
}

/// Plays a turn with the given action of the player (a refused one: the first move instead).
/// @param a  the action
/// @return whether choose() took it
bool turn(const Action& a)
{
   if (!to_action())
   {
      return false;
   }
   const bool ok = choose(g_state, kPlayer, a);
   if (!ok)
   {
      CHECK(choose(g_state, kPlayer, Action{ActionKind::Move, 0, kFoePosition, 0}));
   }
   to_action();
   return ok;
}

/// Salve from the bag: restores and is consumed; at full HP it fails and stays; an item the bag lacks is refused.
void test_item_action()
{
   reset();
   CHECK(inventory::add(g_bag, ITEM_SALVE, kSome));
   uint16_t stats[kMaxStats] = {};
   CHECK(record_stats(g_party.members[0], stats));
   const uint16_t top = stats[STAT_VIGOR];
   monster::set(g_party.members[0], Role::CurrentHp, static_cast<uint32_t>(top - kHurt));
   CHECK(begin(&kData.parties[PARTY_DUMMY]));
   CHECK(to_action());
   CHECK(item_usable(g_state, kPlayer, ITEM_SALVE) && !item_usable(g_state, kPlayer, ITEM_LURE));
   CHECK(!item_usable(g_state, kPlayer, ITEM_ETHER));   // a field item
   CHECK(!choose(g_state, kPlayer, item_action(ITEM_LURE, kFoePosition, 0)));   // none in the bag
   CHECK(!choose(g_state, kPlayer, item_action(ITEM_SALVE, kBench, kFoe)));     // not a member of its party
   const uint16_t before = g_state.battlers[kPlayer].hp;
   CHECK(choose(g_state, kPlayer, item_action(ITEM_SALVE, kBench, kPlayer)));
   to_action();
   CHECK(g_seen.uses == 1 && g_seen.user == kPlayer && g_seen.item == ITEM_SALVE);
   CHECK(inventory::count(g_bag, ITEM_SALVE) == kSome - 1);
   // the Dummy's Tap hit it after the Salve (the item bracket goes first)
   CHECK(g_state.battlers[kPlayer].hp <= before + kSalveHp);
   // full hit points: the logic fails, nothing is consumed
   g_state.battlers[kPlayer].hp = max_hp(g_state.battlers[kPlayer]);
   CHECK(choose(g_state, kPlayer, item_action(ITEM_SALVE, kBench, kPlayer)));
   to_action();
   CHECK(inventory::count(g_bag, ITEM_SALVE) == kSome - 1);
}

/// The odds preview: the rate scaled by the missing HP, then the item and status bonuses.
void test_odds()
{
   reset();
   CHECK(begin(nullptr));
   CHECK(to_action());
   const Battler& t = g_state.battlers[kFoe];
   const RecruitOdds full = recruit_odds_preview(g_state, kPlayer, kFoe, kLureBonus);
   const int32_t at_full = kBlobRate * max_hp(t) / (2 * max_hp(t)) * kLureBonus / kTenths * kNoStatusBonus / kTenths;
   CHECK(full.sure == 0 && full.out_of == kOutOf && full.chance == static_cast<uint32_t>(at_full));
   g_state.battlers[kFoe].status = kSleepy;
   const RecruitOdds sleepy = recruit_odds_preview(g_state, kPlayer, kFoe, kLureBonus);
   CHECK(sleepy.chance == static_cast<uint32_t>(at_full * kSleepyBonus / kNoStatusBonus));
   CHECK(recruit_odds_preview(g_state, kPlayer, kFoe, kSureBonus).sure == 1);
}

/// Snare: sure, every check passes, the record joins the party with what the rules keep.
void test_recruit_to_party()
{
   reset();
   CHECK(inventory::add(g_bag, ITEM_SNARE, 1));
   monster::set(g_wild.members[0], Role::PrimaryStatus, kSleepy);
   CHECK(begin(nullptr));
   CHECK(to_action());
   const uint16_t hp = g_state.battlers[kFoe].hp;
   CHECK(choose(g_state, kPlayer, item_action(ITEM_SNARE, kFoePosition, 0)));
   CHECK(!to_action());
   CHECK(g_state.outcome == Outcome::Recruited && g_state.recruit.battler == kFoe);
   CHECK(g_state.recruit.placed == Placed::Party && g_state.recruit.slot == 1 && g_party.count == 2);
   CHECK(g_seen.checks == kChecks && g_seen.moments[static_cast<int>(RecruitMoment::Recruited)] == 1);
   CHECK(g_seen.moments[static_cast<int>(RecruitMoment::ToParty)] == 1);
   CHECK(inventory::count(g_bag, ITEM_SNARE) == 0);
   const monster::Record& rec = g_party.members[1];
   CHECK(monster::get(rec, Role::CurrentHp) == hp && (monster::get(rec, Role::PrimaryStatus) & 0x1u) == kSleepy);
   CHECK(monster::get(rec, Role::OwnerId) == (kOwner & 0xFFFFu));
   CHECK(monster::get(rec, static_cast<monster::RoleId>(ROLE_ORB_USED)) == ITEM_SNARE);
   CHECK(monster::get(rec, Role::Level) == kWildLevel);
}

/// review4 finding 9: a battler given as data keeps its battle self in the record (its genes, not a new roll) and
/// its kept hit points never exceed the record's maximum.
void test_recruit_data_battler()
{
   reset();
   CHECK(inventory::add(g_bag, ITEM_SNARE, 1));
   Setup su{};
   su.participants[0] = Participant{nullptr, &g_party, nullptr, Controller::Human, 0, 0, 0};
   su.participants[0].bag = &g_bag;
   su.participants[1] = Participant{&kData.parties[PARTY_DUMMY], nullptr, nullptr, Controller::Ai, 1,
                                    static_cast<uint8_t>(1u << PARTY_UNOWNED), 0};
   su.count = 2;
   su.format = FORMAT_SINGLE;
   su.weather = kNone8;
   su.seed = kSeed;
   su.owner_id = kOwner;
   su.storage = &g_storage;
   g_seen = Seen{};
   CHECK(start(g_state, su));
   CHECK(to_action());
   const Battler& foe = g_state.battlers[kFoe];
   g_state.battlers[kFoe].hp = static_cast<uint16_t>(foe.stats[0] + kHurt);   // above its maximum
   uint8_t genes[kMaxStats] = {};
   for (int k = 0; k < kData.stat_count; ++k)
   {
      genes[k] = foe.genes[k];
   }
   CHECK(choose(g_state, kPlayer, item_action(ITEM_SNARE, kFoePosition, 0)));
   CHECK(!to_action() && g_state.outcome == Outcome::Recruited && g_party.count == 2);
   const monster::Record& rec = g_party.members[1];
   bool same = true;
   for (int k = 0; k < kData.stat_count; ++k)
   {
      same = same && monster::get(rec, Role::StatGenes, k) == genes[k];
   }
   CHECK(same);
   monster::Record full = rec;
   fill_resources(full);
   CHECK(monster::get(rec, Role::CurrentHp) <= monster::get(full, Role::CurrentHp));
}

/// Lures until one recruits: an event per check passed, a broke-free moment for each failure, each Lure consumed.
void test_recruit_checks()
{
   reset();
   CHECK(inventory::add(g_bag, ITEM_LURE, kLures));
   CHECK(begin(nullptr));
   int thrown = 0;
   int free = 0;
   while (to_action() && thrown < kLures)
   {
      const int before = g_seen.checks;
      CHECK(choose(g_state, kPlayer, item_action(ITEM_LURE, kFoePosition, 0)));
      ++thrown;
      const bool going = to_action();
      CHECK(g_seen.checks - before == g_state.recruit.passed);
      if (!going)
      {
         break;
      }
      CHECK(g_state.recruit.passed < kChecks);
      ++free;
      CHECK(g_seen.moments[static_cast<int>(RecruitMoment::BrokeFree)] == free);
   }
   CHECK(inventory::count(g_bag, ITEM_LURE) == static_cast<uint32_t>(kLures - thrown));
   CHECK(g_state.outcome == Outcome::None || g_state.outcome == Outcome::Recruited);
   std::printf("recruit tests: %d lures, %d broke free, outcome %d\n", thrown, free,
               static_cast<int>(g_state.outcome));
}

/// A full party: the record goes to the current box's first free slot; no room at all: refused, the item stays.
void test_recruit_to_storage()
{
   reset();
   for (int k = 1; k < kPartySize; ++k)
   {
      make(g_party.members[k], SPECIES_CUBE, kLevel);
   }
   g_party.count = kPartySize;
   CHECK(inventory::add(g_bag, ITEM_SNARE, kSome));
   CHECK(storage::set_current(g_storage, 1));
   CHECK(begin(nullptr));
   CHECK(to_action());
   CHECK(choose(g_state, kPlayer, item_action(ITEM_SNARE, kFoePosition, 0)));
   CHECK(!to_action());
   CHECK(g_state.recruit.placed == Placed::Storage && g_state.recruit.slot == kBoxSize);   // Barn's first slot
   CHECK(storage::count(g_storage) == 1 && storage::box_count(g_storage, 1) == 1);
   CHECK(g_seen.moments[static_cast<int>(RecruitMoment::ToStorage)] == 1);
   monster::Record boxed{};
   CHECK(storage::read(g_storage, kBoxSize, boxed) && monster::get(boxed, Role::Level) == kWildLevel);
   // every slot taken: no room, the battle goes on and keeps the Snare
   for (int k = 0; k < kBoxes * kBoxSize; ++k)
   {
      storage::put(g_storage, k, boxed);
   }
   CHECK(storage::count(g_storage) == kBoxes * kBoxSize && storage::deposit(g_storage, boxed) == storage::kNoSlot);
   g_wild.count = 1;
   CHECK(begin(nullptr));
   CHECK(to_action());
   CHECK(choose(g_state, kPlayer, item_action(ITEM_SNARE, kFoePosition, 0)));
   CHECK(to_action());
   CHECK(g_state.outcome == Outcome::None && g_seen.moments[static_cast<int>(RecruitMoment::NoRoom)] == 1);
   CHECK(inventory::count(g_bag, ITEM_SNARE) == kSome - 1);
}

/// An owned party refuses (the item stays); a battle without recruiting blocks it (the item is used up: C6).
void test_refusals()
{
   reset();
   CHECK(inventory::add(g_bag, ITEM_SNARE, kSome));
   CHECK(begin(&kData.parties[PARTY_DUMMY]));
   CHECK(to_action());
   CHECK(choose(g_state, kPlayer, item_action(ITEM_SNARE, kFoePosition, 0)));
   CHECK(to_action());
   CHECK(g_seen.moments[static_cast<int>(RecruitMoment::Refused)] == 1 && g_state.outcome == Outcome::None);
   CHECK(inventory::count(g_bag, ITEM_SNARE) == kSome);
   CHECK(begin(nullptr, static_cast<uint8_t>(1u << SETUP_NO_RECRUIT)));
   CHECK(to_action());
   CHECK(choose(g_state, kPlayer, item_action(ITEM_SNARE, kFoePosition, 0)));
   CHECK(to_action());
   // C6 (review4 finding 11): the fixture's refusal_consumes names blocked: that Snare is used up
   CHECK(g_seen.moments[static_cast<int>(RecruitMoment::Blocked)] == 1);
   CHECK(inventory::count(g_bag, ITEM_SNARE) == kSome - 1 && (kRecruit.flags & 1u << RECRUIT_CONSUME_BLOCKED) != 0);
   CHECK((kRecruit.flags & 1u << RECRUIT_CONSUME_REFUSED) == 0);
}

/// The careful profile: Salve below half HP, then Beacon (its own ai_score) while asleep; each item used once. The AI
/// chooses at the start of a turn: to_action() returns with that turn's AI action in State::actions.
void test_ai_items()
{
   reset();
   CHECK(begin(&kData.parties[PARTY_KEEPER]));
   CHECK(kData.parties[PARTY_KEEPER].items[0] == ITEM_SALVE && kData.parties[PARTY_KEEPER].items[1] == ITEM_BEACON);
   g_state.battlers[kFoe].hp = kLowHp;
   CHECK(to_action());
   const Action& chosen = g_state.actions[kFoePosition];
   CHECK(chosen.kind == ActionKind::Item && chosen.item == ITEM_SALVE && chosen.battler == kFoe);
   const Action drain_move{ActionKind::Move, 3, kFoePosition, 0};   // Drain: no damage
   CHECK(turn(drain_move));
   CHECK(g_seen.uses == 1 && g_seen.user == kFoe && g_seen.item == ITEM_SALVE);
   CHECK(g_state.parties[1].items[0] == 0 && g_state.battlers[kFoe].hp == kLowHp + kSalveHp);
   // the next turn's choice is made; asleep from here on, Beacon's ai_score beats every move the turn after
   g_state.battlers[kFoe].status = kSleepy;
   g_state.battlers[kFoe].hp = max_hp(g_state.battlers[kFoe]);
   CHECK(turn(drain_move));
   CHECK(g_state.actions[kFoePosition].kind == ActionKind::Item && g_state.actions[kFoePosition].item == ITEM_BEACON);
   CHECK(turn(drain_move));
   CHECK(g_seen.uses == 2 && g_seen.item == ITEM_BEACON && g_state.battlers[kFoe].status == 0);
   CHECK(g_state.parties[1].items[1] == 0);
   // nothing left: low HP uses no item
   g_state.battlers[kFoe].hp = kLowHp;
   CHECK(turn(drain_move) && g_state.actions[kFoePosition].kind != ActionKind::Item);
}

/// The storage module: deposit order, withdraw, move, release, the current box, validity.
void test_storage()
{
   storage::clear(g_storage);
   CHECK(storage::slot_count() == kBoxes * kBoxSize && storage::kStorage.box_count == kBoxes);
   CHECK(storage::box_name(0) != nullptr && storage::box_name(kBoxes) == nullptr);
   monster::Record a{};
   make(a, SPECIES_BLOB, kLevel);
   CHECK(storage::deposit(g_storage, a) == 0 && storage::deposit(g_storage, a) == 1);
   CHECK(storage::set_current(g_storage, 1) && storage::deposit(g_storage, a) == kBoxSize);
   CHECK(!storage::set_current(g_storage, kBoxes));
   CHECK(storage::count(g_storage) == 3 && storage::box_count(g_storage, 0) == 2);
   monster::Record b{};
   CHECK(storage::withdraw(g_storage, 1, b) && !storage::used(g_storage, 1));
   CHECK(monster::get(b, Role::SpeciesKey) == monster::get(a, Role::SpeciesKey));
   CHECK(!storage::withdraw(g_storage, 1, b) && storage::move(g_storage, 0, 1) && storage::used(g_storage, 1));
   CHECK(!storage::move(g_storage, 0, 2) && storage::release(g_storage, 1) && storage::count(g_storage) == 1);
   CHECK(!storage::put(g_storage, kBoxes * kBoxSize, a) && storage::valid(g_storage));
   // C6 (review4 finding 7): the storage is the manifest's size, and every access checks the view's capacity
   CHECK(STORAGE_BYTES == static_cast<uint32_t>(kBoxes * kBoxSize) * monster::kLayout.record_bytes);
   CHECK(STORAGE_USED_BYTES == storage::used_bytes_for(kBoxes * kBoxSize) && storage::fits(g_storage));
   storage::Storage small = g_storage;
   small.capacity = monster::kLayout.record_bytes;   // room for slot 0 only
   CHECK(!storage::fits(small) && !storage::put(small, 1, a) && storage::used(small, 0) == storage::used(g_storage, 0));
   storage::Storage none{};
   CHECK(!storage::fits(none) && storage::deposit(none, a) == storage::kNoSlot && storage::count(none) == 0);
   g_storage.used[g_storage.used_capacity - 1] = 0x80;   // a bit past the build's slots
   CHECK(!storage::valid(g_storage));
}

/// The save image's Boxes section: its size, and the boxes written and read back.
void test_save()
{
   using save::Status;
   const uint32_t slots = kBoxes * kBoxSize;
   CHECK(save::section_size(save::Section::Boxes) ==
         save::kBoxesHeadBytes + storage::used_bytes_for(slots) + slots * monster::kLayout.record_bytes);
   reset();
   monster::Record a{};
   make(a, SPECIES_CUBE, kWildLevel);
   CHECK(storage::put(g_storage, kBoxSize + 1, a) && storage::set_current(g_storage, 1));
   save::Sources src{&g_bag, &g_party, &g_storage, nullptr};
   CHECK(save::write(src) == Status::Ok);
   const uint8_t saved_current = g_storage.current;
   storage::clear(g_storage);
   CHECK(save::read(src) == Status::Ok);
   CHECK(storage::count(g_storage) == 1 && storage::used(g_storage, kBoxSize + 1) && g_storage.current == 1);
   monster::Record back{};
   CHECK(storage::read(g_storage, kBoxSize + 1, back));
   bool same = true;
   for (int k = 0; k < monster::kLayout.record_bytes; ++k)
   {
      same = same && back.bytes[k] == a.bytes[k];
   }
   CHECK(same && saved_current == g_storage.current);
   // C6 (review4 finding 5): a write without the storage copies the newest copy's boxes, it does not wipe them
   const save::Sources no_boxes{&g_bag, &g_party, nullptr, nullptr};
   storage::clear(g_storage);
   CHECK(save::write(no_boxes) == Status::Ok && save::read(src) == Status::Ok && storage::count(g_storage) == 1);
   CHECK(storage::used(g_storage, kBoxSize + 1));
   // and a read without it leaves the boxes as they are
   CHECK(storage::release(g_storage, kBoxSize + 1) && save::read(no_boxes) == Status::Ok);
   CHECK(storage::count(g_storage) == 0);
   CHECK(save::image_size() <= kImageBytes);
}

} // namespace

int main()
{
   test_item_action();
   test_odds();
   test_recruit_to_party();
   test_recruit_data_battler();
   test_recruit_checks();
   test_recruit_to_storage();
   test_refusals();
   test_ai_items();
   test_storage();
   test_save();
   std::printf("recruit tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
