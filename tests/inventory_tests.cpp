/// @file
/// Tests for the inventory (D-26 section 2, G2) against the battle data compiled from tests/data/inventory: the
/// bag over the manifest's pockets (Goods: 4 slots of up to 5, in arrival order; Discs: 3 slots of 1, by item
/// index; Key: 2 slots of 1), all-or-nothing adds and takes, an item with no pocket, sorting; item use out of
/// battle through the use_field hook in a field context (Salve restores 10 HP, Ether 5 PP of a move slot, Point Up
/// raises a slot's PP maximum through its pp_ups role until the role is full, the discs teach a move: at once into
/// a free slot, or by a learn offer with every slot taken, the item consumed only when the move is learned; Disc
/// Tap is not consumed); the engine save image (sections, two copies, checksum, the older copy after a damaged
/// newer one, another shape refused).
#include "capsule/battle/battle.h"
#include "capsule/hal/save.h"
#include "capsule/inventory/bag.h"
#include "capsule/inventory/use.h"
#include "capsule/save/save.h"

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
using namespace capsule::inventory;
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

/// @name The fixture's numbers (tests/data/inventory)
///@{
constexpr uint32_t kGoodsMax = 5;        ///< Goods' quantity_max.
constexpr int kGoodsSlots = 4;           ///< Goods' capacity.
constexpr int32_t kSalveHp = 10;         ///< What Salve restores.
constexpr int32_t kEtherPp = 5;          ///< What Ether restores.
constexpr uint32_t kPpBase = 10;         ///< Every move's pp.
constexpr uint32_t kPpUpsMax = 3;        ///< pp_ups is 2 bits wide.
constexpr uint32_t kPpUpDiv = 5;         ///< pp_max = base + base * ups / 5.
constexpr int kBagSlots = 9;             ///< 4 + 3 + 2 slots.
constexpr int kPockets = 3;              ///< Goods, Discs, Key.
///@}
/// @name The test's own numbers
///@{
constexpr uint32_t kRecordSeed = 777;    ///< Seed of the stream records are made from.
constexpr uint8_t kLevel = 20;           ///< Level of the test creature.
constexpr int32_t kHurt = 15;            ///< Hit points the creature lacks.
constexpr uint32_t kLowPp = 2;           ///< PP left in the slot an Ether refills.
constexpr int kTextCodes = 64;           ///< Codes of one formatted text.
constexpr uint8_t kSlot0 = 0;            ///< First move slot.
constexpr uint8_t kSlot1 = 1;            ///< Second move slot.
constexpr uint8_t kLastSlot = 3;         ///< The fixture's last move slot (4 slots).
constexpr uint8_t kMember = 0;           ///< The party member items are used on.
constexpr uint32_t kSome = 3;            ///< A few of an item.
constexpr uint8_t kDamaged = 0x5A;       ///< A byte written over a saved image.
constexpr size_t kImageBytes = 512;      ///< Room for the fixture's image (239 bytes).
///@}

battle::State g_state;       ///< The lent State of the field contexts (large; static storage).
monster::Party g_party{};    ///< The party.
Bag g_bag{};                 ///< The bag.

/// The fixture's creature in party slot 0 (Spark, Rush, Refresh, Drain; full pools).
void make_party()
{
   g_party = monster::Party{};
   rng::Stream r{kRecordSeed};
   CHECK(battle::make_monster(g_party.members[kMember], SPECIES_BLOB, kLevel, r));
   g_party.count = 1;
}

/// The record's hit points.
/// @return current_hp
uint32_t hp()
{
   return monster::get(g_party.members[kMember], Role::CurrentHp);
}

/// The record's most hit points.
/// @return the vigor stat
uint32_t max_hp()
{
   uint16_t stats[battle::kMaxStats] = {};
   CHECK(battle::record_stats(g_party.members[kMember], stats));
   return stats[STAT_VIGOR];
}

/// Pockets, all-or-nothing adds and takes, the arrival and index orders.
void test_bag()
{
   clear(g_bag);
   CHECK(kInventory.pocket_count == kPockets && kInventory.slot_count == kBagSlots);
   CHECK(add(g_bag, ITEM_SALVE, kSome) && count(g_bag, ITEM_SALVE) == kSome);
   CHECK(!add(g_bag, ITEM_SALVE, kGoodsMax));   // past quantity_max: nothing added
   CHECK(count(g_bag, ITEM_SALVE) == kSome && room_for(g_bag, ITEM_SALVE) == kGoodsMax - kSome);
   CHECK(add(g_bag, ITEM_SALVE, kGoodsMax - kSome) && count(g_bag, ITEM_SALVE) == kGoodsMax);
   CHECK(!remove(g_bag, ITEM_SALVE, kGoodsMax + 1) && count(g_bag, ITEM_SALVE) == kGoodsMax);
   CHECK(!add(g_bag, ITEM_LOOSE_STONE, 1) && room_for(g_bag, ITEM_LOOSE_STONE) == 0);   // no pocket
   CHECK(add(g_bag, ITEM_ETHER, 1) && add(g_bag, ITEM_POINT_UP, 1) && add(g_bag, ITEM_CHARM, 1));
   CHECK(add(g_bag, ITEM_BADGE, 1) && count(g_bag, ITEM_BADGE) == 1);   // another pocket: room
   int used = 0;
   const BagSlot* goods = pocket_slots(g_bag, POCKET_GOODS, used);
   CHECK(used == kGoodsSlots && goods[0].item == ITEM_SALVE && goods[kGoodsSlots - 1].item == ITEM_CHARM);
   // Goods is full (4 distinct items): a fifth kind does not fit; taking all of one closes the gap
   CHECK(remove(g_bag, ITEM_ETHER, 1) && count(g_bag, ITEM_ETHER) == 0);
   goods = pocket_slots(g_bag, POCKET_GOODS, used);
   CHECK(used == kGoodsSlots - 1 && goods[1].item == ITEM_POINT_UP && goods[kGoodsSlots - 1].item == 0);
   // Discs keep item order whatever order they come in
   CHECK(add(g_bag, ITEM_DISC_TAP, 1) && add(g_bag, ITEM_DISC_BURST, 1));
   const BagSlot* discs = pocket_slots(g_bag, POCKET_DISCS, used);
   CHECK(used == 2 && discs[0].item == ITEM_DISC_BURST && discs[1].item == ITEM_DISC_TAP);
   CHECK(!add(g_bag, ITEM_DISC_TAP, 1));   // quantity_max 1
   CHECK(valid(g_bag));
   // a player's sort of Goods by index, then the bag stays consistent
   sort(g_bag, POCKET_GOODS, SortRule::Index);
   goods = pocket_slots(g_bag, POCKET_GOODS, used);
   CHECK(goods[0].item == ITEM_SALVE && goods[1].item == ITEM_POINT_UP && goods[2].item == ITEM_CHARM);
   CHECK(valid(g_bag));
   CHECK(has_flag(ITEM_BADGE, kItemKey) && !has_flag(ITEM_BADGE, kItemConsumed) && !has_flag(ITEM_BADGE, kItemHoldable));
   CHECK(has_flag(ITEM_SALVE, kItemConsumed) && !has_flag(ITEM_DISC_TAP, kItemConsumed));
   CHECK(info(ITEM_SALVE)->price == 10u && info(ITEM_LOOSE_STONE)->pocket == kNoPocket);
}

/// Salve: restores, writes back, consumes; a full or fainted member: no effect.
void test_restore()
{
   make_party();
   clear(g_bag);
   CHECK(add(g_bag, ITEM_SALVE, kSome) && add(g_bag, ITEM_BADGE, 1));
   const uint32_t top = max_hp();
   {
      FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_SALVE) == UseResult::NoEffect);   // full: nothing consumed
      CHECK(u.use(g_bag, ITEM_BADGE) == UseResult::NotUsable);   // a key item with no use
      CHECK(u.use(g_bag, ITEM_LOOSE_STONE) == UseResult::NotUsable);
   }
   CHECK(count(g_bag, ITEM_SALVE) == kSome);
   monster::set(g_party.members[kMember], Role::CurrentHp, top - kHurt);
   {
      FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_SALVE) == UseResult::Used);
      text::Code line[kTextCodes];
      CHECK(u.next_text(line, kTextCodes));   // "{user} feels better!"
      CHECK(!u.next_text(line, kTextCodes));
   }
   CHECK(hp() == top - kHurt + kSalveHp && count(g_bag, ITEM_SALVE) == kSome - 1);
   monster::set(g_party.members[kMember], Role::CurrentHp, 0);
   {
      FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_SALVE) == UseResult::NoEffect);   // fainted
   }
   CHECK(hp() == 0 && count(g_bag, ITEM_SALVE) == kSome - 1);
   {
      FieldUse u(g_state, g_party, kNoMember);
      CHECK(u.use(g_bag, ITEM_SALVE) == UseResult::BadTarget);   // a member item needs a member
   }
   // C6 (review4 finding 10): Mixed Brew restores, then fails: the restore and its text are put back
   monster::set(g_party.members[kMember], Role::CurrentHp, top - kHurt);
   CHECK(add(g_bag, ITEM_MIXED_BREW, 1));
   {
      FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_MIXED_BREW) == UseResult::NoEffect);
      text::Code line[kTextCodes];
      CHECK(!u.next_text(line, kTextCodes));
   }
   CHECK(hp() == top - kHurt && count(g_bag, ITEM_MIXED_BREW) == 1);
}

/// Ether and Point Up on a move slot (the context move of their logic).
void test_move_slot()
{
   make_party();
   clear(g_bag);
   CHECK(add(g_bag, ITEM_ETHER, kSome) && add(g_bag, ITEM_POINT_UP, kGoodsMax));
   monster::Record& rec = g_party.members[kMember];
   monster::set(rec, Role::Pp, kSlot0, kLowPp);
   {
      FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_ETHER) == UseResult::BadTarget);              // no slot chosen
      CHECK(u.use(g_bag, ITEM_ETHER, kSlot0) == UseResult::Used);
      CHECK(u.use(g_bag, ITEM_ETHER, kSlot1) == UseResult::NoEffect);       // full
   }
   CHECK(monster::get(rec, Role::Pp, kSlot0) == kLowPp + kEtherPp && monster::get(rec, Role::Pp, kSlot1) == kPpBase);
   CHECK(count(g_bag, ITEM_ETHER) == kSome - 1);
   for (uint32_t k = 1; k <= kPpUpsMax; ++k)
   {
      FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_POINT_UP, kSlot1) == UseResult::Used);
   }
   {
      FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_POINT_UP, kSlot1) == UseResult::NoEffect);   // the raises role is full
   }
   CHECK(monster::get(rec, Role::PpUps, kSlot1) == kPpUpsMax);
   CHECK(monster::get(rec, Role::Pp, kSlot1) == kPpBase + kPpBase * kPpUpsMax / kPpUpDiv);   // the pool follows
   CHECK(count(g_bag, ITEM_POINT_UP) == kGoodsMax - kPpUpsMax);
   monster::set(rec, Role::Moves, kLastSlot, monster::kNoMove);
   {
      FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_ETHER, kLastSlot) == UseResult::BadTarget);   // an empty slot
   }
}

/// The discs: into a free slot at once, by a learn offer with every slot taken, never twice.
void test_teach()
{
   make_party();
   clear(g_bag);
   CHECK(add(g_bag, ITEM_DISC_BURST, 1) && add(g_bag, ITEM_DISC_TAP, 1));
   monster::Record& rec = g_party.members[kMember];
   // every slot taken: the offer waits; keeping every move keeps the disc
   {
      FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_DISC_BURST) == UseResult::LearnOffer && u.offered_move() == MOVE_BURST);
      CHECK(u.use(g_bag, ITEM_DISC_TAP) == UseResult::NotUsable);   // one offer at a time
      CHECK(u.answer_learn(g_bag, battle::kKeepMoves) == UseResult::NoEffect);
   }
   CHECK(count(g_bag, ITEM_DISC_BURST) == 1 && monster::get(rec, Role::Moves, kSlot0) == MOVE_SPARK);
   // forgetting slot 0: learned, written back with a full pool, the disc consumed
   monster::set(rec, Role::Pp, kSlot0, kLowPp);
   {
      FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_DISC_BURST) == UseResult::LearnOffer);
      CHECK(u.answer_learn(g_bag, kSlot0) == UseResult::Used);
   }
   CHECK(monster::get(rec, Role::Moves, kSlot0) == MOVE_BURST && monster::get(rec, Role::Pp, kSlot0) == kPpBase);
   CHECK(count(g_bag, ITEM_DISC_BURST) == 0);
   // a free slot learns at once; Disc Tap is not consumed; a known move is no effect
   monster::set(rec, Role::Moves, kLastSlot, monster::kNoMove);
   {
      FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_DISC_TAP) == UseResult::Used);
      CHECK(u.use(g_bag, ITEM_DISC_TAP) == UseResult::NoEffect);
   }
   CHECK(monster::get(rec, Role::Moves, kLastSlot) == MOVE_TAP && count(g_bag, ITEM_DISC_TAP) == 1);
   // C6 (review4 finding 23): Blob lists Burst and Tap as teachable; Cube lists none, so it learns neither
   CHECK(add(g_bag, ITEM_DISC_BURST, 1));
   rng::Stream r{kRecordSeed};
   CHECK(battle::make_monster(g_party.members[kMember], SPECIES_CUBE, kLevel, r));
   {
      FieldUse u(g_state, g_party, kMember);
      CHECK(u.use(g_bag, ITEM_DISC_BURST) == UseResult::NoEffect);
   }
   CHECK(count(g_bag, ITEM_DISC_BURST) == 1);
}

/// The save image: sizes, write and read back, two copies, a damaged newer copy, another shape.
void test_save()
{
   using save::Status;
   CHECK(save::section_size(save::Section::Bag) == save::kBagHeadBytes + kBagSlots * save::kBagEntryBytes);
   CHECK(save::section_size(save::Section::Party) ==
         save::kPartyHeadBytes + monster::kLayout.party_size * monster::kLayout.record_bytes);
   CHECK(save::section_size(save::Section::Boxes) == 0 && save::section_size(save::Section::Script) == 0);
   CHECK(save::image_size() <= save::kSaveBudget && save::copy_count() == 2);
   save::Sources src{&g_bag, &g_party, nullptr, nullptr};
   uint32_t seq = 0;
   CHECK(save::newest(seq) == Status::Empty && save::read(src) == Status::Empty);
   make_party();
   clear(g_bag);
   CHECK(add(g_bag, ITEM_SALVE, kSome) && add(g_bag, ITEM_DISC_TAP, 1) && add(g_bag, ITEM_BADGE, 1));
   CHECK(save::write(src) == Status::Ok && save::newest(seq) == Status::Ok && seq == 1);
   const monster::Party saved = g_party;
   // a second image (one Salve less) goes to the other copy
   CHECK(remove(g_bag, ITEM_SALVE, 1) && save::write(src) == Status::Ok && save::newest(seq) == Status::Ok && seq == 2);
   clear(g_bag);
   g_party = monster::Party{};
   CHECK(save::read(src) == Status::Ok);
   CHECK(count(g_bag, ITEM_SALVE) == kSome - 1 && count(g_bag, ITEM_DISC_TAP) == 1 && count(g_bag, ITEM_BADGE) == 1);
   CHECK(valid(g_bag) && g_party.count == saved.count);
   bool same = true;
   for (int b = 0; b < monster::kLayout.record_bytes; ++b)
   {
      same = same && g_party.members[kMember].bytes[b] == saved.members[kMember].bytes[b];
   }
   CHECK(same);
   // damage the newer copy (copy 1, after the header): the older one is read
   const uint32_t copy_one = save::slot_bytes();   // C7: copy 1's slot, half the medium
   CHECK(hal::save::program(copy_one + save::kHeaderBytes, &kDamaged, 1));
   CHECK(save::newest(seq) == Status::Ok && seq == 1);
   CHECK(save::read(src) == Status::Ok && count(g_bag, ITEM_SALVE) == kSome);
   // both damaged: nothing changes
   CHECK(hal::save::program(save::kHeaderBytes, &kDamaged, 1));
   clear(g_bag);
   CHECK(save::read(src) == Status::Corrupt && count(g_bag, ITEM_SALVE) == 0);
   // a fresh write goes to copy 0 (no valid copy), then reads
   CHECK(add(g_bag, ITEM_SALVE, 1) && save::write(src) == Status::Ok && save::read(src) == Status::Ok);
   CHECK(count(g_bag, ITEM_SALVE) == 1);
}

/// The image byte for byte against the one tools/save_layout.py writes (tests/data/inventory/save_image.bin: a bag
/// of three Salves, Disc Tap and the Badge, no party, sequence 1 in copy 0).
/// @param path  the Python image
void test_python_image(const char* path)
{
   std::FILE* f = std::fopen(path, "rb");
   CHECK(f != nullptr);
   if (f == nullptr)
   {
      return;
   }
   uint8_t expected[kImageBytes] = {};
   const size_t n = std::fread(expected, 1, sizeof(expected), f);
   std::fclose(f);
   CHECK(n > 0 && n <= save::image_size());   // the filled bag entries only: smaller than the most
   CHECK(hal::save::erase(0, hal::save::capacity()));
   clear(g_bag);
   CHECK(add(g_bag, ITEM_SALVE, kSome) && add(g_bag, ITEM_DISC_TAP, 1) && add(g_bag, ITEM_BADGE, 1));
   CHECK(save::write(save::Sources{&g_bag, nullptr, nullptr, nullptr}) == save::Status::Ok);
   uint8_t got[kImageBytes] = {};
   CHECK(hal::save::read(0, got, static_cast<uint32_t>(n)));
   bool same = n > 0;
   for (size_t k = 0; k < n; ++k)
   {
      same = same && got[k] == expected[k];
   }
   CHECK(same);
}

} // namespace

/// Runs the tests.
/// @param argc  2: the Python image's path follows
/// @param argv  arguments
/// @return 0 when every check passed
int main(int argc, char** argv)
{
   test_bag();
   test_restore();
   test_move_slot();
   test_teach();
   test_save();
   if (argc > 1)
   {
      test_python_image(argv[1]);
   }
   std::printf("inventory tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
