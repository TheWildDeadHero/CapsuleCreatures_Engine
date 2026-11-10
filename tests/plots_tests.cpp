/// @file
/// M6a K2: the plots (capsule/plots/plots.h; B-5) on the engine library's headless HAL, whose real-time clock the
/// test sets (capsule/hal/rtc.h set_for_test()), so the economy clock moves by whole hours:
///   - a new game's start crops, ripe; plot_at() and look();
///   - growth through the stages, watering and the yield, the ripe time, the regrowth cycles (their watering starts
///     over), withering;
///   - a tree's replant on picking; planting refused (occupied, another type, no such crop);
///   - Wait and an RTC set back change nothing;
///   - K3: a crop's item, the script port's Item and At kinds;
///   - the save's Plots section: a round trip, a crop the build lacks, a world without engine records.
#include "capsule/core/clock.h"
#include "capsule/hal/rtc.h"
#include "capsule/hal/system.h"
#include "capsule/plots/plots.h"
#include "capsule/save/save.h"
#include "capsule/script/port.h"

#include <cstdint>
#include <cstdio>

namespace
{

namespace clock = capsule::clock;
namespace plots = capsule::plots;
namespace rtc = capsule::hal::rtc;
namespace save = capsule::save;
using capsule::AssetKey;

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

constexpr AssetKey kSoilSprite = AssetKey{0x1001u};   ///< The soil's empty sprite.
constexpr AssetKey kOranSprite = AssetKey{0x2002u};   ///< The soil crop's sprite.
constexpr AssetKey kTreeSprite = AssetKey{0x3003u};   ///< The tree crop's sprite.

constexpr uint8_t kSoil = 0;              ///< Type 0: soil ...
constexpr uint8_t kSoilStages = 4;        ///< ... of four stages, watered ...
constexpr uint16_t kSoilRipeHours = 6;    ///< ... ripe six hours ...
constexpr uint8_t kSoilRegrow = 1;        ///< ... regrowing once.
constexpr uint8_t kTree = 1;              ///< Type 1: a tree, one stage, ripe until picked, replanted.
constexpr uint16_t kOran = 1;             ///< Crop 1 (soil): ...
constexpr uint16_t kOranHours = 3;        ///< ... three hours a stage ...
constexpr uint8_t kOranMin = 2;           ///< ... two to ...
constexpr uint8_t kOranMax = 6;           ///< ... six items.
constexpr uint16_t kApple = 2;            ///< Crop 2 (tree): ...
constexpr uint16_t kAppleHours = 24;      ///< ... a day to grow back, ...
constexpr uint8_t kAppleYield = 3;        ///< ... three items.
constexpr uint16_t kItemOran = 7;         ///< The items they yield.
constexpr uint16_t kItemApple = 8;        ///< (the second)
constexpr uint16_t kMapA = 2;             ///< The places' maps ...
constexpr uint16_t kMapB = 5;             ///< ... (two of them).
constexpr uint16_t kSoilPlot = 0;         ///< Plot 0: soil on map A, object 1.
constexpr uint16_t kTreePlot = 1;         ///< Plot 1: a tree on map A, object 4, an apple from the start.
constexpr uint16_t kFarPlot = 2;          ///< Plot 2: soil on map B, object 0.
constexpr uint8_t kSoilLocal = 1;         ///< Plot 0's object.
constexpr uint8_t kTreeLocal = 4;         ///< Plot 1's object.
constexpr uint16_t kPlaceCount = 3;       ///< Places.

constexpr plots::TypeData kTypes[] = {
   {kSoilSprite, kSoilRipeHours, kSoilStages, kSoilRegrow, 1, 0, {}},
   {AssetKey{}, 0, 1, 0, 0, 1, {}},
};
constexpr plots::CropData kCrops[] = {
   {kOranSprite, kItemOran, kOranHours, kSoil, kOranMin, kOranMax, {}},
   {kTreeSprite, kItemApple, kAppleHours, kTree, kAppleYield, kAppleYield, {}},
};
constexpr plots::Place kPlaces[] = {
   {kMapA, kSoilLocal, kSoil, plots::kNoCrop, {}},
   {kMapA, kTreeLocal, kTree, kApple, {}},
   {kMapB, 0, kSoil, plots::kNoCrop, {}},
};
plots::PlotRecord g_records[kPlaceCount];   ///< The engine's records.
plots::Data g_data = {kTypes, kCrops, kPlaces, 2, 2, kPlaceCount, g_records};
bool g_has_plots = true;   ///< data() answers g_data (else nullptr: a world without plots).

constexpr uint8_t kPeriods[] = {0};                     ///< One period.
constexpr uint8_t kMaxWait = 23;                        ///< Hours a Wait adds at most.
constexpr uint32_t kCooldown = 60;                      ///< A minute between Waits.
const clock::Settings kSettings = {kPeriods, sizeof(kPeriods), 0, 0, kMaxWait, kCooldown, 1,
                                   clock::SeasonMode::FixedDays, 1};
constexpr uint32_t kHour = clock::kSecondsPerHour;      ///< An hour's seconds.
constexpr uint32_t kStart = 366u * clock::kSecondsPerDay;   ///< The RTC at the new game: 2001-01-01.
uint32_t g_now = kStart;                                ///< The RTC's time now.

/// Moves the RTC on and lets the clock read it.
/// @param hours  hours forward (the RTC set back: wrap with a large value)
void pass(uint32_t hours)
{
   g_now += hours * kHour;
   rtc::set_for_test(rtc::from_seconds_since_2000(g_now), true);
   clock::resume();
}

/// A new game at kStart.
void new_game()
{
   g_now = kStart;
   rtc::set_for_test(rtc::from_seconds_since_2000(g_now), true);
   clock::begin_new();
   plots::begin_new();
}

/// A new game's start crop, plot_at(), look().
void test_start()
{
   new_game();
   CHECK(plots::state(kTreePlot) == 1 && plots::crop(kTreePlot) == kApple && plots::yield(kTreePlot) == kAppleYield);
   CHECK(plots::state(kSoilPlot) == plots::kStageEmpty && plots::crop(kSoilPlot) == plots::kNoCrop);
   CHECK(plots::state(kPlaceCount) == plots::kStageEmpty);   // no such plot
   CHECK(plots::plot_at(kMapA, kSoilLocal) == kSoilPlot && plots::plot_at(kMapA, kTreeLocal) == kTreePlot);
   CHECK(plots::plot_at(kMapB, 0) == kFarPlot && plots::plot_at(kMapA, 0) == plots::kNoPlot);
   AssetKey sprite{};
   uint8_t anim = 0;
   CHECK(plots::look(kMapA, kTreeLocal, sprite, anim) && sprite == kTreeSprite && anim == 1);
   CHECK(plots::look(kMapA, kSoilLocal, sprite, anim) && sprite == kSoilSprite && anim == 0);   // empty soil
   CHECK(!plots::look(kMapB, 1, sprite, anim));
}

/// Soil: the stages, watering, the yield, the ripe time, a regrowth (its watering starts over), withering.
void test_growth()
{
   new_game();
   CHECK(!plots::plant(kSoilPlot, kApple));             // a tree's crop
   CHECK(!plots::plant(kSoilPlot, kApple + 1));         // no such crop
   CHECK(plots::plant(kSoilPlot, kOran));
   CHECK(!plots::plant(kSoilPlot, kOran));              // occupied
   CHECK(plots::state(kSoilPlot) == 0 && plots::yield(kSoilPlot) == 0 && plots::pick(kSoilPlot) == 0);
   CHECK(plots::water(kSoilPlot));                      // stage 0
   pass(kOranHours);
   CHECK(plots::state(kSoilPlot) == 1);
   pass(kOranHours);
   CHECK(plots::state(kSoilPlot) == 2 && plots::water(kSoilPlot));   // stage 2
   pass(2 * kOranHours);
   CHECK(plots::state(kSoilPlot) == kSoilStages && !plots::water(kSoilPlot));   // ripe: no more watering
   CHECK(plots::yield(kSoilPlot) == kOranMin + (kOranMax - kOranMin) * 2 / kSoilStages);
   AssetKey sprite{};
   uint8_t anim = 0;
   CHECK(plots::look(kMapA, kSoilLocal, sprite, anim) && sprite == kOranSprite && anim == kSoilStages);
   // left ripe past its ripe time: it regrows, unwatered
   pass(kSoilRipeHours);
   CHECK(plots::state(kSoilPlot) == 0 && plots::crop(kSoilPlot) == kOran);
   pass(kSoilStages * kOranHours);
   CHECK(plots::state(kSoilPlot) == kSoilStages && plots::yield(kSoilPlot) == kOranMin);
   // its last cycle's ripe time over: withered, and it can be planted again
   pass(kSoilRipeHours);
   CHECK(plots::state(kSoilPlot) == plots::kStageEmpty && plots::crop(kSoilPlot) == plots::kNoCrop);
   CHECK(plots::plant(kSoilPlot, kOran) && plots::state(kSoilPlot) == 0);
   plots::clear(kSoilPlot);
   CHECK(plots::state(kSoilPlot) == plots::kStageEmpty);
}

/// Picking: soil empties, a tree grows again; a tree takes no water.
void test_pick()
{
   new_game();
   CHECK(!plots::water(kTreePlot));
   CHECK(plots::pick(kTreePlot) == kAppleYield);
   CHECK(plots::state(kTreePlot) == 0 && plots::crop(kTreePlot) == kApple && plots::pick(kTreePlot) == 0);
   pass(kAppleHours);
   CHECK(plots::state(kTreePlot) == 1);
   CHECK(plots::plant(kFarPlot, kOran));
   pass(kSoilStages * kOranHours);
   CHECK(plots::pick(kFarPlot) == kOranMin && plots::state(kFarPlot) == plots::kStageEmpty);
}

/// M6a K3: a plot's crop's item; the Item and At kinds through the engine library's script port.
void test_item()
{
   new_game();
   CHECK(plots::item(kTreePlot) == kItemApple && plots::item(kSoilPlot) == 0);
   CHECK(plots::plant(kSoilPlot, kOran) && plots::item(kSoilPlot) == kItemOran);
   using capsule::script::PlotOp;
   CHECK(capsule::script::world::plot_op(PlotOp::Item, kTreePlot, 0) == kItemApple);
   CHECK(capsule::script::world::plot_op(PlotOp::At, kSoilLocal, 0) == capsule::script::kNoPlot);   // no map shown
}

/// Wait moves only the world clock; an RTC set back credits nothing.
void test_economy_only()
{
   new_game();
   CHECK(plots::plant(kSoilPlot, kOran));
   pass(kOranHours - 1);
   CHECK(clock::wait(kMaxWait));
   CHECK(plots::state(kSoilPlot) == 0);
   g_now -= 2 * kOranHours * kHour;   // the RTC set back
   pass(0);
   CHECK(plots::state(kSoilPlot) == 0);
   pass(3 * kOranHours);   // one hour past where it was before
   CHECK(plots::state(kSoilPlot) == 1);
}

/// The save's Plots section.
void test_save()
{
   CHECK(save::section_size(save::Section::Plots) == save::kPlotsHeadBytes + kPlaceCount * plots::kRecordBytes);
   new_game();
   CHECK(plots::plant(kSoilPlot, kOran) && plots::water(kSoilPlot));
   const plots::PlotRecord soil = g_records[kSoilPlot];
   const plots::PlotRecord tree = g_records[kTreePlot];
   save::Sources s{nullptr, nullptr, nullptr, nullptr};
   s.plots = g_records;
   CHECK(save::write(s) == save::Status::Ok);
   plots::clear(kSoilPlot);
   plots::clear(kTreePlot);
   CHECK(save::read(s) == save::Status::Ok);
   const plots::PlotRecord& back = g_records[kSoilPlot];
   CHECK(back.crop == soil.crop && back.planted == soil.planted && back.watered == soil.watered &&
         back.cycle == soil.cycle && back.start_stage == soil.start_stage);
   CHECK(g_records[kTreePlot].crop == tree.crop && g_records[kTreePlot].start_stage == tree.start_stage);
   // a build without the tree's crop: that record loads empty
   g_data.crop_count = 1;
   CHECK(save::read(s) == save::Status::Ok && g_records[kTreePlot].crop == plots::kNoCrop);
   CHECK(g_records[kSoilPlot].crop == kOran);
   g_data.crop_count = 2;
   // a world whose game keeps its own plots: no section; an image without one loads every plot empty
   g_data.records = nullptr;
   CHECK(save::section_size(save::Section::Plots) == 0 && plots::state(kSoilPlot) == plots::kStageEmpty);
   CHECK(save::write(s) == save::Status::Ok);
   g_data.records = g_records;
   CHECK(save::read(s) == save::Status::Ok && g_records[kSoilPlot].crop == plots::kNoCrop);
   g_has_plots = false;
   CHECK(save::section_size(save::Section::Plots) == 0 && plots::plot_at(kMapA, kSoilLocal) == plots::kNoPlot);
   g_has_plots = true;
}

}   // namespace

/// The test world's plots.
/// @return g_data, or nullptr while g_has_plots is false
const capsule::plots::Data* capsule::plots::data()
{
   return g_has_plots ? &g_data : nullptr;
}

/// The test world's clock (Wait needs one).
/// @return kSettings
const capsule::clock::Settings* capsule::clock::settings()
{
   return &kSettings;
}

int main()
{
   capsule::hal::system::init();
   test_start();
   test_growth();
   test_pick();
   test_item();
   test_economy_only();
   test_save();
   std::printf("plots tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
