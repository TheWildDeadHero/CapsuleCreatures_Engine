/// @file
/// UC (D-33): the screen framework on the software video model: the task list's order, the fade's calls (the
/// reference games' normal fade: 0 -> 16 in 23 calls, BG and OBJ halves on alternate calls, the blend an arithmetic
/// shift), sprite animation commands (frames, jumps, loops), the draw order and the tile allocator, the layers'
/// addressing, fills and copies, the stack's frame timing (open fade, hand-over, setup, in fade, close, the field's
/// four return steps) and the return to the field: the map's view, tiles, camera and edited cells, the field sprite's
/// tiles and the palettes as they were, without entering the map again; U6's one-layer setup, mode change and
/// in-place sheet tiles; UM's screen arena: zeroed, value-initialised, aligned blocks, last in first out, stale leases,
/// scoped blocks, and its ties to the stack (a screen's blocks go back on its pop or replace(), a parent keeps or gives
/// back its own around a child, the field's block stays).
#include "capsule/asset/asset.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/map/map.h"
#include "capsule/screen/arena.h"
#include "capsule/screen/fade.h"
#include "capsule/screen/layers.h"
#include "capsule/screen/screen.h"
#include "capsule/screen/sprites.h"
#include "capsule/screen/tasks.h"

#include "../src/hal/soft/soft_video.h"

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
}   // namespace capsule::hal::system

namespace
{

using namespace capsule;
namespace video = hal::video;
namespace sp = screen::sprites;

int g_checks = 0;     ///< Checks run.
int g_failures = 0;   ///< Checks failed.

/// Counts a check.
/// @param ok    its result
/// @param what  its text
/// @param line  its line
void check(bool ok, const char* what, int line)
{
   ++g_checks;
   if (!ok)
   {
      ++g_failures;
      std::printf("FAIL line %d: %s\n", line, what);
   }
}
#define CHECK(x) check((x), #x, __LINE__)

/// Fresh video, queue, palettes and modules.
void fresh()
{
   hal::soft::reset();
   hal::vram::reset();
   gfx::palette::reset();
   gfx::palette::set_rounding(gfx::palette::Rounding::Truncate);
   gfx::sprites::reset();
   screen::reset();
   screen::tasks::reset();
   screen::layers::reset();
   sp::reset();
}

// ---- tasks ----

char g_ran[16];   ///< Task names in run order.
int g_ran_count = 0;   ///< Entries of g_ran.

void note(char c)
{
   if (g_ran_count < static_cast<int>(sizeof(g_ran)))
   {
      g_ran[g_ran_count++] = c;
   }
}

void task_d(uint8_t) { note('D'); }
void task_e(uint8_t) { note('E'); }
void task_b(uint8_t) { note('B'); }
void task_c(uint8_t id)
{
   note('C');
   screen::tasks::destroy(id);   // a task that destroys itself: the run goes on past it
}
void task_a(uint8_t id)
{
   note('A');
   if (screen::tasks::task(id).data[0]++ == 0)
   {
      screen::tasks::create(task_d, 9);   // after the current one: runs this frame
      screen::tasks::create(task_e, 0);   // before it: next frame
   }
}

bool ran(const char* expect)
{
   int n = 0;
   while (expect[n] != '\0')
   {
      if (n >= g_ran_count || g_ran[n] != expect[n])
      {
         return false;
      }
      ++n;
   }
   return n == g_ran_count;
}

void tasks()
{
   fresh();
   namespace t = screen::tasks;
   const uint8_t a = t::create(task_a, 5);
   const uint8_t b = t::create(task_b, 2);
   const uint8_t c = t::create(task_c, 5);
   CHECK(a == 0 && b == 1 && c == 2);
   g_ran_count = 0;
   t::run();
   CHECK(ran("BACD"));   // priority order, ties in creation order; D (9) created during A runs this frame
   g_ran_count = 0;
   t::run();
   CHECK(ran("EBAD"));   // E (0) waited for this frame; C destroyed itself
   CHECK(t::count() == 4 && t::find(task_c) == t::kNoTask && t::find(task_e) != t::kNoTask);
   const uint8_t again = t::create(task_c, 1);
   CHECK(again == c);    // the lowest free entry
   for (int i = t::count(); i < t::kMaxTasks; ++i)
   {
      t::create(task_b, 1);
   }
   CHECK(t::create(task_b, 1) == t::kNoTask);
}

// ---- fades ----

static_assert(gfx::palette::blend_floor(31, 0, 2) == 27, "31 + ((0 - 31) * 2 >> 4): -62 >> 4 = -4");
static_assert(gfx::palette::blend(31, 0, 2) == 28, "the engine's default rounds toward zero");
static_assert(gfx::palette::blend_floor(0, 31, 2) == 3, "62 >> 4 = 3 (same as truncation)");

void fades()
{
   fresh();
   namespace f = screen::fade;
   const video::Color red = video::rgb5(video::kChannelMax, 0, 0);
   gfx::palette::load_bg(1, &red, 1);
   CHECK(f::begin(f::kToBlack));   // call 1: the BG half at y 0
   CHECK(!f::begin(f::kFromBlack));   // refused while one runs
   int calls = 1;
   while (f::update())
   {
      ++calls;
      if (calls == 3)
      {
         // call 3 blended the BG half at y 2 while the OBJ half is still at 0
         CHECK(gfx::palette::bank_coef(0) == 2 && gfx::palette::bank_coef(gfx::palette::kObjBankShift) == 0);
         hal::vram::flush_all();
         // the arithmetic shift: 31 -> 27 (truncation would give 28)
         CHECK(hal::soft::peek16(video::Region::BgPalette, video::kHalfwordBytes) == video::rgb5(27, 0, 0));
      }
      if (calls == 17)
      {
         CHECK(gfx::palette::bank_coef(0) == f::kFull && gfx::palette::bank_coef(gfx::palette::kObjBankShift) == 14);
      }
   }
   ++calls;   // the call that ended it
   CHECK(calls == 23);   // 9 levels x 2 halves, then 4 finishing calls and the last one
   CHECK(f::level() == f::kFull && !f::active());
   // a negative delay: step 2 + 2, no waiting: 0, 4, 8, 12, 16 -> 10 calls + 5
   CHECK(f::begin({f::kAllBanks, -2, f::kFull, f::kClear, f::kBlack}));
   calls = 1;
   while (f::update())
   {
      ++calls;
   }
   CHECK(calls + 1 == 15 && gfx::palette::bank_coef(0) == 0);
   // a delay of 1: each step waits a call on the BG half
   CHECK(f::begin({f::kAllBanks, 1, f::kClear, f::kFull, f::kBlack}));
   calls = 1;
   while (f::update())
   {
      ++calls;
   }
   CHECK(calls + 1 == 31);   // 9 levels x (2 + 1 wait) - 1 (the first does not wait) + 5
}

// ---- sprites ----

constexpr sp::AnimCmd kWalk[] = {
   {sp::AnimOp::Frame, 3, 0, 0, 0}, {sp::AnimOp::Frame, 2, sp::kFlipH, 0, 4}, {sp::AnimOp::Jump, 0, 0, 0, 0}};
constexpr sp::AnimCmd kLooped[] = {
   {sp::AnimOp::Frame, 1, 0, 0, 1}, {sp::AnimOp::Loop, 0, 0, 0, 0}, {sp::AnimOp::Frame, 1, 0, 0, 2},
   {sp::AnimOp::Frame, 1, 0, 0, 3}, {sp::AnimOp::Loop, 0, 0, 0, 1}, {sp::AnimOp::Frame, 1, 0, 0, 5},
   {sp::AnimOp::End, 0, 0, 0, 0}};
constexpr const sp::AnimCmd* kAnims[] = {kWalk, kLooped};
constexpr uint16_t kSheetTag = 100;   ///< The sheet's tag.
constexpr uint16_t kPaletteTag = 200;   ///< The palette's tag.
constexpr sp::Template kSheetSprite = {kSheetTag, kPaletteTag, video::OBJ_SQUARE, 1, 1, 0, sp::kAffineOff,
                                       video::ObjMode::Normal, 0, kAnims, nullptr, nullptr};
constexpr uint32_t kFrameWords[2 * 4 * 8] = {};   ///< Two 16 x 16 frames of an images sprite.
constexpr sp::Images kImages = {kFrameWords, 4, 2};
constexpr sp::Template kImageSprite = {sp::kNoTag, sp::kNoTag, video::OBJ_SQUARE, 1, 2, 3, sp::kAffineOff,
                                       video::ObjMode::Normal, 0, nullptr, &kImages, nullptr};

void sprites()
{
   fresh();
   const asset::Tiles* sheet = asset::tiles(asset::key_v1("tiles:SCREEN_SHEET"));
   const asset::Palette* pal = asset::palette(asset::key_v1("palette:SCREEN_BG"));
   CHECK(sheet != nullptr && sheet->count == 8 && pal != nullptr && pal->count == 18);
   CHECK(sp::load_sheet(kSheetTag, *sheet) == 0);
   CHECK(sp::load_palette(kPaletteTag, pal->colors) == 0 && sp::load_palette(kPaletteTag, pal->colors) == 0);
   const uint8_t walker = sp::create(kSheetSprite, 100, 50, 4);
   CHECK(walker == 0 && sp::sprite(walker).center_x == -8);
   int tiles[6];
   for (int &t : tiles)
   {
      sp::animate();
      t = sp::sprite(walker).tile;
   }
   CHECK(tiles[0] == 0 && tiles[1] == 0 && tiles[2] == 0 && tiles[3] == 4 && tiles[4] == 4 && tiles[5] == 0);
   sp::start_anim(walker, 1);
   int looped[7];
   for (int &t : looped)
   {
      sp::animate();
      t = sp::sprite(walker).tile;
   }
   CHECK(looped[0] == 1 && looped[1] == 2 && looped[2] == 3 && looped[3] == 2 && looped[4] == 3 &&
         looped[5] == 5 && looped[6] == 5 && sp::sprite(walker).anim_ended);
   // an images sprite gets a frame's tiles after the sheet (first fit)
   const uint8_t icon = sp::create(kImageSprite, 40, 60, 4);
   CHECK(icon == 1 && sp::sprite(icon).sheet_start == 8 && sp::tiles_used() == 12);
   // draw order: equal priority and subpriority: the lower sprite on screen first; then a better priority
   sp::sprite(icon).priority = 1;
   sp::build_oam();
   CHECK(sp::drawn(0) == icon && sp::drawn(1) == walker && sp::drawn(2) == sp::kNoSprite);
   sp::sprite(icon).subpriority = 5;
   sp::build_oam();
   CHECK(sp::drawn(0) == walker && sp::drawn(1) == icon);
   sp::destroy(icon);
   CHECK(sp::tiles_used() == 8 && sp::count() == 1);
}

// ---- layers ----

constexpr screen::layers::LayerTemplate kLayerSet[] = {{0, 0, 31, 0, 0, 0, 0}, {1, 1, 26, 1, 0, 1, 16}};
constexpr screen::layers::Plan kPlan = {kLayerSet, 2, 0};
alignas(4) uint16_t g_map0[32 * 32];   ///< BG0's buffer.
alignas(4) uint16_t g_map1[64 * 32];   ///< BG1's buffer (64 x 32).

void layers()
{
   fresh();
   namespace l = screen::layers;
   l::apply(kPlan);
   l::set_buffer(0, g_map0);
   l::set_buffer(1, g_map1);
   CHECK(l::map_bytes(1) == 4096 && l::width_cells(1) == 64 && l::cell_index(1, 40, 3) == 1024 + 3 * 32 + 8);
   l::fill(1, video::map_cell(5, 0, true), 30, 0, 4, 1, 2, 1);
   CHECK(g_map1[30] == (0x400 | 5 | 2 << 12) && g_map1[31] == (0x400 | 6 | 2 << 12) &&
         g_map1[1024] == (0x400 | 7 | 2 << 12) && g_map1[1025] == (0x400 | 8 | 2 << 12));
   const uint16_t src[2] = {0x0003, 0x0804};
   g_map0[0] = 0x7400;
   l::copy_rect(0, src, 0, 0, 2, 0, 0, 2, 1, l::kPaletteKeep, 1, 1);
   CHECK(g_map0[0] == (0x7400 + 0x1000 + 4) && g_map0[1] == (0x1000 + 5));
   l::copy_rect(0, src, 0, 0, 2, 0, 1, 2, 1, 3, 0, 0);
   CHECK(g_map0[32] == (0x3000 | 3) && g_map0[33] == (0x3000 | 0x804));
   l::copy_linear(1, src, 40, 0, 2, 1);
   CHECK(g_map1[40] == 3);   // linear: rows of 32 whatever the size (the reference games' rectangle copy)
   const asset::Tilemap* map = asset::tilemap(asset::key_v1("tilemap:SCREEN_BG"));
   CHECK(map != nullptr && map->count == 4);
   l::set_tilemap(0, *map, 64);
   l::schedule_copy(0);
   l::copy_scheduled();
   hal::vram::flush_all();
   CHECK(hal::soft::peek16(video::Region::BgVram, 31 * video::kScreenBlockBytes + 66 * 2) == 4097);
   const asset::Tiles* tiles = asset::tiles(asset::key_v1("tiles:SCREEN_BG"));
   l::load_tiles(1, *tiles, 2);   // BG1's first tile is 16: tiles 18-19 of block 1
   CHECK(hal::soft::peek16(video::Region::BgVram, video::kCharBlockBytes + 18 * 32) == 0x1111 &&
         hal::soft::peek16(video::Region::BgVram, video::kCharBlockBytes + 19 * 32) == 0x2222);
}

// ---- U6: one layer's template, the mode, a sheet's tiles in place ----

constexpr screen::layers::LayerTemplate kAffineLayer = {2, 2, 6, 2, 1, 2, 0};   ///< A 64 x 64 8bpp map.

void u6_additions()
{
   fresh();
   namespace l = screen::layers;
   l::apply(kPlan);
   l::set_buffer(0, g_map0);
   l::set_mode(video::kModeMixed);   // SetBgMode(1): the text layers keep their templates and buffers
   CHECK(l::buffer(0) == g_map0 && l::layer(1) != nullptr && l::layer(1)->screen_size == 1);
   l::init(kAffineLayer);   // InitBgFromTemplate: one layer, the others kept
   CHECK(l::layer(2) != nullptr && l::layer(2)->bpp8 == 1 && l::layer(2)->screen_size == 2 && l::buffer(2) == nullptr);
   CHECK(l::buffer(0) == g_map0 && l::map_bytes(2) == 64 * 64);
   l::set_mode(0);
   CHECK(l::layer(2) != nullptr && l::buffer(0) == g_map0);
   // a sheet's tiles replaced in place (through the queue: they land at the flush)
   const asset::Tiles* sheet = asset::tiles(asset::key_v1("tiles:SCREEN_SHEET"));
   CHECK(sheet != nullptr && sp::load_sheet(kSheetTag, *sheet) == 0);
   const uint32_t words[asset::kTileWords4bpp] = {0x33333333u, 0x33333333u, 0x33333333u, 0x33333333u,
                                                  0x33333333u, 0x33333333u, 0x33333333u, 0x33333333u};
   sp::copy_tiles(3, words, 1);
   sp::copy_tiles(static_cast<uint16_t>(sp::kTiles), words, 1);   // past sprite memory: nothing
   hal::vram::flush_all();
   CHECK(hal::soft::peek16(video::Region::ObjVram, 3 * 32) == 0x3333 && sp::sheet_start(kSheetTag) == 0);
}

// ---- U3: a sheet's tiles taken without loading (the reference games' reserved tiles) ----

constexpr uint16_t kReservedTag = 101;     ///< The reserved sheet's tag.
constexpr uint16_t kReservedTiles = 16;    ///< Its tiles.
constexpr uint16_t kAfterTag = 102;        ///< A sheet loaded after it.

void u3_additions()
{
   fresh();
   // reserved first: tiles 0-15 taken, nothing written; the next sheet goes past them (first fit)
   CHECK(sp::reserve_sheet(kReservedTag, kReservedTiles) == 0);
   CHECK(sp::sheet_start(kReservedTag) == 0);
   const asset::Tiles* sheet = asset::tiles(asset::key_v1("tiles:SCREEN_SHEET"));
   CHECK(sheet != nullptr && sp::load_sheet(kAfterTag, *sheet) == kReservedTiles);
   // the screen writes the reserved tiles itself
   const uint32_t words[asset::kTileWords4bpp] = {0x55555555u, 0x55555555u, 0x55555555u, 0x55555555u,
                                                  0x55555555u, 0x55555555u, 0x55555555u, 0x55555555u};
   sp::copy_tiles(static_cast<uint16_t>(sp::sheet_start(kReservedTag) + 1), words, 1);
   hal::vram::flush_all();
   CHECK(hal::soft::peek16(video::Region::ObjVram, 32) == 0x5555);
   // freed: the tiles are free again; no room: kNoTiles
   sp::free_sheet(kReservedTag);
   CHECK(sp::sheet_start(kReservedTag) == sp::kNoTiles);
   CHECK(sp::reserve_sheet(kReservedTag, kReservedTiles) == 0);
   CHECK(sp::reserve_sheet(kAfterTag + 1, static_cast<uint16_t>(sp::kTiles)) == sp::kNoTiles);
}

// ---- the stack and the return to the field ----

uint32_t g_setup_frames[4];   ///< The frames of the first setup calls.
uint32_t g_last_setup = 0;    ///< The frame of the last setup call.
int g_setup_calls = 0;        ///< Setup calls.
int g_leaves = 0;             ///< leave() calls.
bool g_resumed = false;       ///< The last setup was a resume.
uint32_t g_frame = 0;         ///< The test's frame counter.

/// A two-call setup that draws over the map's video memory (BG1: its tiles' block, its screen block).
bool test_setup(uint8_t& state, uint8_t, bool resumed)
{
   if (g_setup_calls < 4)
   {
      g_setup_frames[g_setup_calls] = g_frame;
   }
   ++g_setup_calls;
   g_last_setup = g_frame;
   g_resumed = resumed;
   if (state++ == 0)
   {
      screen::layers::set_buffer(1, g_map1);
      screen::layers::fill_linear(1, 0x0003, 0, 0, 32, 32);
      screen::layers::copy_to_vram(1);
      screen::layers::load_tiles(1, *asset::tiles(asset::key_v1("tiles:SCREEN_BG")), 0);
      const asset::Palette* pal = asset::palette(asset::key_v1("palette:SCREEN_BG"));
      gfx::palette::load_bg(0, pal->colors, pal->count);
      gfx::palette::load_obj(0, pal->colors, pal->count);
      return false;
   }
   screen::layers::show(1, true);
   return true;
}

void test_leave(bool) { ++g_leaves; }

bool g_close_in_main = false;   ///< C13a: the child's main calls close_now() once.
int g_closing_vblanks = 0;      ///< C13a: the child's vertical blank functions after its close_now().

/// C13a: the child's main: close_now() when asked (a task's SetMainCallback2).
void child_main()
{
   if (g_close_in_main)
   {
      g_close_in_main = false;
      screen::close_now();
   }
}

/// C13a: the child's vertical blank function: counts the ones that run after its close_now().
void child_vblank()
{
   if (screen::phase() == screen::Phase::Closing)
   {
      ++g_closing_vblanks;
   }
}

/// C13a: the child's frame: tasks, then its main (where it closes).
constexpr screen::Step kChildSteps[] = {screen::Step::Tasks, screen::Step::Main};

constexpr screen::layers::LayerTemplate kOverMap[] = {{1, 0, 28, 1, 0, 0, 0}};   ///< The map's block and BG1 ring.
constexpr screen::layers::Plan kOverMapPlan = {kOverMap, 1, 0};
const screen::Screen kTestScreen = {&kOverMapPlan, test_setup, nullptr, nullptr, nullptr, test_leave, nullptr, 0,
                                    screen::kFadeToBlack, screen::kFadeFromBlack, screen::kFadeToBlack};
const screen::Screen kChildScreen = {&kOverMapPlan, test_setup, nullptr, child_main, child_vblank, test_leave,
                                     kChildSteps, 2, screen::kNoTransition, screen::kNoTransition,
                                     screen::kNoTransition};

constexpr uint32_t kMapBytes = 3 * video::kScreenBlockBytes;   ///< The map's three rings (blocks 28-30).
uint16_t g_before_rings[kMapBytes / 2];   ///< The rings before.
uint16_t g_before_tiles[64 * 16];         ///< The map's first tiles before.
uint16_t g_before_frame[video::kScreenWidth * video::kScreenHeight];   ///< The composed screen before.
uint16_t g_after_frame[video::kScreenWidth * video::kScreenHeight];    ///< ... after the return.

void run_frame()
{
   ++g_frame;
   screen::frame(ui::Keys{});
   hal::vram::flush_all();
}

void stack_and_return()
{
   fresh();
   gfx::palette::set_rounding(gfx::palette::Rounding::Floor);
   const map::Map* field = map::find(asset::key_v1("map:FIELD"));
   CHECK(field != nullptr);
   map::enter(*field, 16, 0);
   const map::MetatileId edit[map::kMaxLayers] = {1, 1, 1, 1};
   CHECK(map::set_cell(2, 1, map::cell_at(*field, 2, 1), edit));
   map::redraw();
   // C13a (R9 M4): the shown field's rings take no tiles; its tile block does
   CHECK(!screen::layers::tiles_clear_of_maps(29 * video::kScreenBlockBytes + 64, 32));
   CHECK(screen::layers::tiles_clear_of_maps(0, 32) && hal::vram::tiles_allowed(0, 32));
   const asset::Sprite* walker = asset::sprite(asset::key_v1("sprite:SCREEN_WALKER"));
   gfx::sprites::load(0, *walker);
   gfx::sprites::show(0, true);
   hal::vram::flush_all();
   for (uint32_t i = 0; i < kMapBytes / 2; ++i)
   {
      g_before_rings[i] = hal::soft::peek16(video::Region::BgVram, 28 * video::kScreenBlockBytes + i * 2);
   }
   const int tile_halfwords = field->tile_count * 16 < 1024 ? field->tile_count * 16 : 1024;
   for (int i = 0; i < tile_halfwords; ++i)
   {
      g_before_tiles[i] = hal::soft::peek16(video::Region::BgVram, static_cast<uint32_t>(i * 2));
   }
   gfx::sprites::set_position(0, 40, 40);
   gfx::sprites::update();
   hal::vram::flush_all();
   hal::soft::latch();
   hal::soft::compose(g_before_frame);
   const uint16_t obj_tile = hal::soft::peek16(video::Region::ObjVram, 3 * 2 + 32);
   const video::Color bg1 = gfx::palette::base(false, 1);
   const video::Color obj1 = gfx::palette::base(true, 1);

   g_frame = 0;
   g_setup_calls = 0;
   g_leaves = 0;
   CHECK(screen::push(kTestScreen, 7));   // the open fade's first call now (frame 0)
   CHECK(screen::phase() == screen::Phase::Opening && screen::active());
   while (g_setup_calls == 0 && g_frame < 100)
   {
      run_frame();
   }
   // the fade's 23 calls end in frame 22; frame 23 hands over; frame 24 the field gives up and setup calls first
   CHECK(g_setup_frames[0] == 24 && map::suspended());
   run_frame();
   CHECK(g_setup_calls == 2 && g_setup_frames[1] == 25 && screen::phase() == screen::Phase::Running);
   CHECK(hal::soft::peek16(video::Region::BgVram, 28 * video::kScreenBlockBytes) == 0x0003);   // drawn over
   // C13a (R9 M4): the map suspended, the screen's own BG1 map (screen block 28, 64 x 32 cells: 28-29) is in use
   CHECK(!screen::layers::tiles_clear_of_maps(29 * video::kScreenBlockBytes, 4) &&
         screen::layers::tiles_clear_of_maps(30 * video::kScreenBlockBytes, 4));

   // a child without fades: the frame after push_now() the parent leaves and the child's setup calls first
   CHECK(screen::push_now(kChildScreen));
   run_frame();
   CHECK(g_leaves == 1 && g_setup_calls == 3 && screen::depth() == 2);
   run_frame();
   CHECK(screen::phase() == screen::Phase::Running && g_setup_calls == 4);
   // C13a: close_now() in the child's frame (its main): its frame's steps go on (its vertical blank function runs)
   // and it leaves at that frame's end; the parent's resumed setup makes its first call the next frame
   g_close_in_main = true;
   g_closing_vblanks = 0;
   run_frame();
   CHECK(g_leaves == 2 && screen::depth() == 2 && screen::phase() == screen::Phase::Left && g_closing_vblanks == 1);
   const uint32_t resumed_at = g_frame + 1;
   run_frame();   // the parent's setup again, resumed
   CHECK(screen::depth() == 1 && g_resumed && g_setup_calls == 5 && g_last_setup == resumed_at);
   run_frame();
   CHECK(screen::phase() == screen::Phase::Running);

   // closing: the out fade (23 calls: the first at close()), leave the frame after, then the field's four steps
   while (screen::fade::active())
   {
      run_frame();   // its in fade first
   }
   const uint32_t closed_at = g_frame;
   screen::close();
   uint32_t left_at = 0;
   while (screen::active() && g_frame < closed_at + 100)
   {
      run_frame();
      if (left_at == 0 && g_leaves == 3)
      {
         left_at = g_frame;
      }
   }
   CHECK(left_at == closed_at + 23);   // the fade's last call in frame closed_at + 22
   CHECK(g_frame == left_at + 4);      // states 0-3 of the return in the four frames after, the last returns true
   CHECK(!map::suspended() && map::current() == field && map::camera_x() == 16);
   CHECK(map::metatile_at(*field, 0, 2, 1) == 1);   // the edited cell is still edited
   CHECK(gfx::palette::bank_coef(0) == screen::fade::kFull);   // still black: the field's own fade-in follows
   // the field's frame: the sprite's entry written again; the colours shown (the field's own fade-in, here at once)
   gfx::palette::set_fade(0, 0, 0);
   gfx::sprites::update();
   hal::vram::flush_all();
   hal::soft::latch();
   hal::soft::compose(g_after_frame);
   bool shown = true;
   for (int i = 0; i < video::kScreenWidth * video::kScreenHeight; ++i)
   {
      shown = shown && g_before_frame[i] == g_after_frame[i];
   }
   CHECK(shown);   // the same picture
   bool same = true;
   constexpr uint32_t kViewRowCells = 22 * 32;   // the drawn rows of a ring (11 metatile rows)
   for (uint32_t i = 0; i < kMapBytes / 2; ++i)
   {
      if (i % 1024 >= kViewRowCells)
      {
         continue;   // rows outside the view: not drawn (the reference games' buffer has zeros there)
      }
      const uint16_t now = hal::soft::peek16(video::Region::BgVram, 28 * video::kScreenBlockBytes + i * 2);
      if (same && g_before_rings[i] != now)
      {
         std::printf("ring %u: %04X now %04X\n", static_cast<unsigned>(i), g_before_rings[i], now);
      }
      same = same && g_before_rings[i] == now;
   }
   CHECK(same);   // the view drawn again as it was
   for (int i = 0; i < tile_halfwords; ++i)
   {
      const uint16_t now = hal::soft::peek16(video::Region::BgVram, static_cast<uint32_t>(i * 2));
      if (same && g_before_tiles[i] != now)
      {
         std::printf("tile word %d: %04X now %04X (tiles %d)\n", i, g_before_tiles[i], now, field->tile_count);
      }
      same = same && g_before_tiles[i] == now;
   }
   CHECK(same);   // the map's tiles back
   CHECK(hal::soft::peek16(video::Region::ObjVram, 3 * 2 + 32) == obj_tile);   // the walker's tiles back
   CHECK(gfx::palette::base(false, 1) == bg1 && gfx::palette::base(true, 1) == obj1);
   CHECK(gfx::palette::bank_coef(0) == 0);
}

// ---- UM: the screen arena ----

namespace arena = screen::arena;

/// A block with a member initialiser that is not zero (as a screen's "no sprite" value).
struct Marked
{
   uint8_t none = 0xFF;    ///< Not zero: T() applies it over the zeroed bytes.
   uint8_t bytes[15];      ///< Zeroed (16 B in all: the next block needs no padding).
};

/// A block that needs more than the arena's word alignment on hosts that have it.
struct Wide
{
   uint64_t value;   ///< 8-byte aligned on most hosts.
};

/// A screen's buffers (a parent).
struct ParentState
{
   uint16_t map[512];   ///< 1 KB.
};

/// A child's buffers.
struct ChildState
{
   uint32_t words[64];   ///< 256 B.
};

screen::Block<Marked> g_field_block;      ///< Taken while no screen is open.
screen::Block<ParentState> g_parent;      ///< The parent screen's.
screen::Block<ChildState> g_child;        ///< The child's.
bool g_parent_frees = false;              ///< The parent gives its block back in leave(true).
uint32_t g_parent_at = 0;                 ///< used() when the parent's setup first ran.

/// The parent's setup: takes its block (again after a child when it gave it back).
bool parent_setup(uint8_t&, uint8_t, bool)
{
   if (!g_parent.taken())
   {
      g_parent_at = arena::used();
      g_parent.take().map[0] = 1;
   }
   return true;
}

/// The parent's leave: gives its block back for a child when it rebuilds on return.
/// @param for_child  a child opens
void parent_leave(bool for_child)
{
   if (for_child && g_parent_frees)
   {
      g_parent.release();
   }
}

/// The child's setup: takes its block.
bool child_setup(uint8_t&, uint8_t, bool)
{
   g_child.take().words[0] = 2;
   return true;
}

const screen::Screen kParentScreen = {nullptr, parent_setup, nullptr, nullptr, nullptr, parent_leave, nullptr, 0,
                                      screen::kNoTransition, screen::kNoTransition, screen::kNoTransition};
const screen::Screen kArenaChild = {nullptr, child_setup, nullptr, nullptr, nullptr, nullptr, nullptr, 0,
                                    screen::kNoTransition, screen::kNoTransition, screen::kNoTransition};
constexpr screen::Host kQuietHost = {nullptr, nullptr, nullptr, nullptr, nullptr};   ///< No field to give up.

/// Runs frames until the stack is empty (at most a few).
void run_until_empty()
{
   constexpr int kMostFrames = 8;
   for (int i = 0; i < kMostFrames && screen::active(); ++i)
   {
      run_frame();
   }
}

void arena_blocks()
{
   fresh();
   CHECK(arena::capacity() == 32768 && arena::used() == 0 && arena::blocks() == 0);   // the default size
   screen::Block<Marked> a;
   Marked& m = a.take();
   CHECK(a.taken() && m.none == 0xFF && m.bytes[0] == 0 && m.bytes[14] == 0);   // zeroed, then T()
   CHECK(arena::used() == sizeof(Marked) && arena::blocks() == 1);
   screen::Block<uint8_t> odd;
   odd.take();   // 1 B: the next block starts on the next word (or the host's wider alignment)
   screen::Block<Wide> w;
   w.take().value = 5;
   CHECK(reinterpret_cast<uintptr_t>(w.get()) % alignof(Wide) == 0);   // aligned (padding after the first block)
   screen::Block<ChildState> c;
   c.take();
   const uint32_t high = arena::used();
   w.release();   // w and every block after it
   CHECK(!w.taken() && !c.taken() && odd.taken() && arena::used() == sizeof(Marked) + 1 && arena::blocks() == 2);
   odd.release();
   CHECK(a.taken() && arena::used() == sizeof(Marked) && arena::blocks() == 1);
   c.release();   // no longer live: nothing
   CHECK(a.taken() && arena::high_water() == high);
   {
      screen::ScopedBlock<ParentState> scratch;
      scratch->map[3] = 4;
      CHECK(arena::blocks() == 2 && scratch->map[0] == 0);
   }
   CHECK(arena::blocks() == 1 && arena::used() == sizeof(Marked));   // given back at the end of its scope
   CHECK(arena::fits(arena::capacity() - arena::used()) && !arena::fits(arena::capacity() - arena::used() + 1));
   a.release();
   CHECK(arena::used() == 0 && !a.taken());
   CHECK(a.take().none == 0xFF && arena::blocks() == 1);   // taken again after its release
}

void arena_stack()
{
   fresh();
   screen::set_host(kQuietHost);
   g_field_block.take();
   const uint32_t field_end = arena::used();

   // a parent that keeps its block around a child (its state survives the child)
   g_parent_frees = false;
   CHECK(screen::push_now(kParentScreen));
   run_frame();
   CHECK(g_parent.taken() && g_parent_at == field_end && g_parent->map[0] == 1);
   const uint32_t parent_end = arena::used();
   CHECK(screen::push_now(kArenaChild));
   run_frame();   // the parent leaves (true) and the child's setup takes its block above the parent's
   CHECK(g_child.taken() && g_parent.taken() && arena::used() == parent_end + sizeof(ChildState));
   screen::close_now();   // C13a: between frames: it leaves at once
   CHECK(screen::phase() == screen::Phase::Left);
   run_frame();   // popped: its block went back; the parent resumes with its own
   CHECK(!g_child.taken() && g_parent.taken() && arena::used() == parent_end && screen::depth() == 1);
   g_parent->map[1] = 9;

   // the same parent giving its block back for the child (pret's free-and-rebuild)
   g_parent_frees = true;
   CHECK(screen::push_now(kArenaChild));
   run_frame();
   CHECK(!g_parent.taken() && g_child.taken() && arena::used() == field_end + sizeof(ChildState));
   screen::close_now();
   run_frame();   // the parent's resumed setup takes a fresh block
   CHECK(g_parent.taken() && g_parent->map[1] == 0 && arena::used() == parent_end);

   // replace(): the old screen's blocks go back and the new one starts at the same mark
   screen::replace(kArenaChild);
   run_frame();   // no out fade: it leaves
   run_frame();   // popped, the replacement's setup
   CHECK(!g_parent.taken() && g_child.taken() && arena::used() == field_end + sizeof(ChildState));
   // C13a: replace_now(): no out fade; it leaves at once (between frames), the replacement's setup the next frame
   screen::replace_now(kArenaChild);
   CHECK(screen::phase() == screen::Phase::Left);   // its block goes back at the pop
   run_frame();
   CHECK(g_child.taken() && arena::used() == field_end + sizeof(ChildState) && screen::depth() == 1);
   screen::close_now();
   run_until_empty();
   CHECK(!screen::active() && !g_child.taken() && g_field_block.taken() && arena::used() == field_end);

   g_field_block.release();
   CHECK(arena::used() == 0 && arena::blocks() == 0);
   screen::reset();
   CHECK(arena::high_water() == 0);
   screen::set_host(screen::default_host());
}

}   // namespace

int main()
{
   tasks();
   fades();
   sprites();
   layers();
   u6_additions();
   u3_additions();
   stack_and_return();
   arena_blocks();
   arena_stack();
   std::printf("screen tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
