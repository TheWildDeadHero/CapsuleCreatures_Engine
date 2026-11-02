/// @file
/// BF (D-38): the scene kit on the software video model: affine sprite animation (frames with and without a
/// duration, loops, jumps, End, pauses, the per-matrix state, double size, the matrix rule, frame by frame), the
/// per-scanline register tables in the compositor (layer scroll, 32-bit scroll pairs, layer control, the affine
/// reference point's restart, window edges with the hardware's wrap and clamps, blend weights; the table off again),
/// the scanline module's two tables and its wave, palette blends (ranges, banks, tags, the unfaded form, ramps), the
/// animation bank reader (header, tables, code check) and the player (frame-exact waits, visual and sound counts,
/// calls, arguments, game commands, binding by name, errors).
#include "capsule/anim/bank.h"
#include "capsule/anim/player.h"
#include "capsule/core/trig.h"
#include "capsule/gfx/palette.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/screen/affine.h"
#include "capsule/screen/blend.h"
#include "capsule/screen/layers.h"
#include "capsule/screen/scanline.h"
#include "capsule/screen/sprites.h"
#include "capsule/screen/tasks.h"

#include "../src/hal/soft/soft_video.h"

#include <cstdio>
#include <cstdlib>

extern const uint8_t kTestAnimBank[];     ///< tests/data/anim/test_bank.canim, assembled at build time.
extern const uint32_t kTestAnimBank_size; ///< Its bytes.

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
namespace af = screen::affine;
namespace tasks = screen::tasks;

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

constexpr int kW = video::kScreenWidth;    ///< Screen width.
constexpr int kH = video::kScreenHeight;   ///< Screen height.
uint16_t g_frame[kW * kH];                 ///< The composed screen.

/// Fresh video, queue, palettes and modules.
void fresh()
{
   hal::soft::reset();
   hal::vram::reset();
   gfx::palette::reset();
   tasks::reset();
   screen::layers::reset();
   sp::reset();
   af::reset();
   screen::scanline::clear();
}

/// Lands the queue, latches and composes a frame (the soft HAL's update()).
void show()
{
   hal::vram::flush_all();
   hal::soft::latch();
   hal::soft::compose(g_frame);
}

/// A composed pixel.
/// @param x  column
/// @param y  row
/// @return BGR555
uint16_t px(int x, int y)
{
   return g_frame[y * kW + x];
}

// --- affine animation ---------------------------------------------------------------------------------------

/// A 32x32 sprite's tiles: every pixel colour 1 except a 4-pixel band at the top in colour 2 (16 tiles).
uint32_t g_square_tiles[16 * 8];

/// Fills g_square_tiles.
void make_square_tiles()
{
   constexpr int kSide = 32;
   constexpr int kTilesWide = 4;
   constexpr int kBand = 4;
   constexpr int kNibbleBits = 4;
   for (uint32_t& w : g_square_tiles)
   {
      w = 0;
   }
   for (int y = 0; y < kSide; ++y)
   {
      for (int x = 0; x < kSide; ++x)
      {
         const uint32_t c = y < kBand ? 2u : 1u;
         const int tile = (y / 8) * kTilesWide + x / 8;
         const int word = tile * 8 + (y % 8);
         g_square_tiles[word] |= c << ((x % 8) * kNibbleBits);
      }
   }
}

/// The affine test animation: absolute scale 1, a growing turn for 2 frames, a loop of a shrinking step repeated
/// twice, End.
const af::Cmd kGrow[] = {af::frame(0x100, 0x100, 0, 0), af::frame(0x20, 0, 4, 2), af::loop(0),
                         af::frame(-0x10, 0x10, -1, 1), af::loop(2),             af::end()};
/// A jump animation: a frame, then back to it forever.
const af::Cmd kSpin[] = {af::frame(0, 0, 8, 1), af::jump(0)};
/// The tables.
const af::Cmd* const kAnims[] = {kGrow, kSpin};

/// The test sprite's template (double size, a sheet).
const sp::Template kSquare = {1,       sp::kNoTag, video::OBJ_SQUARE, 2,       1, 3, sp::kAffineDouble,
                              video::ObjMode::Normal, 0,  nullptr,          nullptr, nullptr};

/// A state's values.
/// @param m   the matrix
/// @param xs  x scale
/// @param ys  y scale
/// @param r   rotation
/// @return true when they match
bool state_is(uint8_t m, int16_t xs, int16_t ys, uint16_t r)
{
   const af::State& st = af::state(m);
   return st.x_scale == xs && st.y_scale == ys && st.rotation == r;
}

/// The matrix the engine's rule gives (written out here: the formula of affine.h).
/// @param xs  x scale
/// @param ys  y scale
/// @param r   rotation
/// @return the matrix
af::Matrix expected_matrix(int16_t xs, int16_t ys, uint16_t r)
{
   const int32_t xi = af::inverse_scale(xs);
   const int32_t yi = af::inverse_scale(ys);
   const int32_t s = sin_q14(r);
   const int32_t c = cos_q14(r);
   return af::Matrix{static_cast<int16_t>((xi * c) >> kQ14Shift), static_cast<int16_t>((-xi * s) >> kQ14Shift),
                     static_cast<int16_t>((yi * s) >> kQ14Shift), static_cast<int16_t>((yi * c) >> kQ14Shift)};
}

/// Whether a matrix is the expected one.
/// @param m   the matrix
/// @param xs  x scale
/// @param ys  y scale
/// @param r   rotation
/// @return true when it is
bool matrix_is(uint8_t m, int16_t xs, int16_t ys, uint16_t r)
{
   const af::Matrix a = af::get(m);
   const af::Matrix e = expected_matrix(xs, ys, r);
   return a.pa == e.pa && a.pb == e.pb && a.pc == e.pc && a.pd == e.pd;
}

/// Affine animation commands, frame by frame (create_and_animate() runs the first step at once).
void affine_commands()
{
   fresh();
   CHECK(af::inverse_scale(0x100) == 0x100 && af::inverse_scale(0x200) == 0x80 && af::inverse_scale(0) == 0);
   CHECK(af::inverse_scale(0x110) == 240);
   const uint8_t id = sp::create_and_animate(kSquare, 120, 80, 0, af::prepare, kAnims);
   CHECK(id == 0);
   const uint8_t m = sp::sprite(id).matrix;
   CHECK(m == 0 && af::taken(0));
   CHECK(sp::sprite(id).center_x == -32 && sp::sprite(id).center_y == -32);   // double size: twice the half box
   // step 1 (at creation): the absolute frame
   CHECK(state_is(m, 0x100, 0x100, 0) && matrix_is(m, 0x100, 0x100, 0));
   sp::animate();   // step 2: the 2-frame turn's first frame
   CHECK(state_is(m, 0x120, 0x100, 0x400) && af::state(m).delay == 1);
   sp::animate();   // step 3: its second frame
   CHECK(state_is(m, 0x140, 0x100, 0x800) && af::state(m).delay == 0);
   sp::animate();   // step 4: the loop marker, then the shrinking step in the same frame
   CHECK(state_is(m, 0x130, 0x110, 0x700));
   sp::animate();   // step 5: loop 2 starts: back to the step
   CHECK(state_is(m, 0x120, 0x120, 0x600) && af::state(m).loop == 2);
   sp::animate();   // step 6: one repeat counted
   CHECK(state_is(m, 0x110, 0x130, 0x500) && af::state(m).loop == 1);
   sp::animate();   // step 7: the last count: on to End
   CHECK(state_is(m, 0x110, 0x130, 0x500) && sp::sprite(id).affine_ended);
   CHECK(matrix_is(m, 0x110, 0x130, 0x500));
   sp::animate();   // End holds
   CHECK(state_is(m, 0x110, 0x130, 0x500) && af::state(m).cmd == 4);

   // a pause keeps the state; disabled steps do nothing
   af::start(id, 1);
   sp::animate();   // begin: rotation 8 steps, delay 0
   CHECK(state_is(m, 0x100, 0x100, 0x800));
   sp::animate();   // jump back to the frame: applied again
   CHECK(state_is(m, 0x100, 0x100, 0x1000));
   sp::sprite(id).affine_paused = true;
   sp::animate();
   CHECK(state_is(m, 0x100, 0x100, 0x1000));
   sp::sprite(id).affine_paused = false;
   af::set_enabled(false);
   sp::animate();
   CHECK(state_is(m, 0x100, 0x100, 0x1000));
   af::set_enabled(true);
   sp::animate();
   CHECK(state_is(m, 0x100, 0x100, 0x1800));

   // change() keeps the scales; start_if_different() leaves a running animation alone
   af::change(id, 0);
   sp::animate();
   CHECK(state_is(m, 0x100, 0x100, 0) && !sp::sprite(id).affine_ended);   // anim 0 begins with its absolute frame
   af::start_if_different(id, 0);
   CHECK(!sp::sprite(id).affine_beginning);

   // matrices: first free, given back as identity; detach makes the sprite plain
   const uint8_t m2 = af::alloc_matrix();
   CHECK(m2 == 1);
   af::free_matrix(m2);
   CHECK(!af::taken(1) && af::get(1).pa == af::kScaleOne && af::get(1).pb == 0);
   af::detach(id);
   CHECK(!af::taken(0) && sp::sprite(id).affine_mode == sp::kAffineOff);
   af::set_mode(id, sp::kAffineNormal);
   CHECK(sp::sprite(id).center_x == -16);

   // the rule is replaceable (a game's exact table)
   af::set_matrix_rule([](int16_t, int16_t, uint16_t) { return af::Matrix{1, 2, 3, 4}; });
   af::set_rotation_scaling(3, 0x100, 0x100, 0);
   CHECK(af::get(3).pa == 1 && af::get(3).pd == 4);
   af::set_matrix_rule(nullptr);
   af::set_rotation_scaling(3, 0x100, 0x100, 0);
   CHECK(af::get(3).pa == af::kScaleOne && af::get(3).pb == 0);
}

/// The hardware's affine sprite drawing, written out for the test: the box (twice the size when double), the
/// texture position of a screen pixel from the box's centre through the matrix.
/// @param ox      box left
/// @param oy      box top
/// @param size    sprite side
/// @param dbl     double size
/// @param x       the matrix
/// @param sx      screen x
/// @param sy      screen y
/// @param[out] tx texture x
/// @param[out] ty texture y
/// @return true when the pixel is inside the sprite
bool reference_texel(int ox, int oy, int size, bool dbl, const af::Matrix& x, int sx, int sy, int& tx, int& ty)
{
   const int box = dbl ? size * 2 : size;
   if (sx < ox || sx >= ox + box || sy < oy || sy >= oy + box)
   {
      return false;
   }
   const int dx = sx - ox - box / 2;
   const int dy = sy - oy - box / 2;
   tx = ((x.pa * dx + x.pb * dy) >> kQ8Shift) + size / 2;
   ty = ((x.pc * dx + x.pd * dy) >> kQ8Shift) + size / 2;
   return tx >= 0 && ty >= 0 && tx < size && ty < size;
}

/// An animated double-size sprite composed pixel by pixel as the reference drawing says.
void affine_pixels()
{
   fresh();
   make_square_tiles();
   CHECK(sp::load_sheet_words(1, g_square_tiles, 16) == 0);
   constexpr video::Color kBack = video::rgb5(0, 0, 8);
   constexpr video::Color kBody = video::rgb5(31, 0, 0);
   constexpr video::Color kBand = video::rgb5(0, 31, 0);
   const video::Color bank[16] = {0, kBody, kBand};
   gfx::palette::load_obj(3 * 16, bank, 16);
   gfx::palette::load_bg(0, &kBack, 1);
   const uint8_t id = sp::create_and_animate(kSquare, 100, 70, 0, af::prepare, kAnims);
   for (int f = 0; f < 3; ++f)
   {
      sp::animate();
   }
   sp::build_oam();
   show();
   const uint8_t m = sp::sprite(id).matrix;
   const af::Matrix x = af::get(m);
   int bad = 0;
   int inside = 0;
   for (int sy = 0; sy < kH; ++sy)
   {
      for (int sx = 0; sx < kW; ++sx)
      {
         int tx = 0;
         int ty = 0;
         const bool in = reference_texel(100 - 32, 70 - 32, 32, true, x, sx, sy, tx, ty);
         const video::Color want = !in ? kBack : (ty < 4 ? kBand : kBody);
         inside += in ? 1 : 0;
         bad += px(sx, sy) != want ? 1 : 0;
      }
   }
   CHECK(bad == 0);
   CHECK(inside > 32 * 32 / 2);   // the sprite is there (scaled 1.25 x 1.0625, turned)
}

// --- scanline tables in the compositor ----------------------------------------------------------------------

/// A BG tile of 8 columns 1..8 (colour index = column + 1).
uint32_t g_columns_tile[8];
/// A BG tile of one colour (index 9).
uint32_t g_solid_tile[8];

/// Sets up BG0 (column tiles, palette 0) and BG1 (solid, under it), mode 0.
void two_layers()
{
   constexpr int kNibbleBits = 4;
   for (int r = 0; r < 8; ++r)
   {
      g_columns_tile[r] = 0;
      g_solid_tile[r] = 0;
      for (int c = 0; c < 8; ++c)
      {
         g_columns_tile[r] |= static_cast<uint32_t>(c + 1) << (c * kNibbleBits);
         g_solid_tile[r] |= 9u << (c * kNibbleBits);
      }
   }
   video::Color pal[16];
   for (int i = 0; i < 16; ++i)
   {
      pal[i] = video::rgb5(static_cast<uint8_t>(i * 2), static_cast<uint8_t>(31 - i), 4);
   }
   video::load_bg_palette(0, pal, 16);
   video::load_bg_tiles(0, 1, g_columns_tile, 1);
   video::load_bg_tiles(0, 2, g_solid_tile, 1);
   video::set_mode(0);
   video::configure_bg(0, true, 0, 0, 30, false, 0);
   video::configure_bg(1, true, 1, 0, 31, false, 0);
   for (int y = 0; y < 32; ++y)
   {
      for (int x = 0; x < 32; ++x)
      {
         video::write_map_cell(0, x, y, video::map_cell(1, 0));
         video::write_map_cell(1, x, y, video::map_cell(2, 0));
      }
   }
}

/// The colour of BG0's column c.
/// @param c  0-7
/// @return the colour
video::Color column_color(int c)
{
   return video::rgb5(static_cast<uint8_t>((c + 1) * 2), static_cast<uint8_t>(31 - (c + 1)), 4);
}

alignas(4) uint16_t g_lines16[video::kLineEntries];   ///< A 16-bit table.
alignas(4) uint32_t g_lines32[video::kLineEntries];   ///< A 32-bit table.

/// Layer scroll per line, 16 and 32 bits, and the table off again.
void scanline_scroll()
{
   fresh();
   two_layers();
   for (int y = 0; y < video::kLineEntries; ++y)
   {
      g_lines16[y] = static_cast<uint16_t>(y % 8);   // line y scrolled y % 8 pixels
   }
   video::set_line_effect(video::LineRegister::Bg0Hofs, video::LineWidth::Half, g_lines16);
   show();
   int bad = 0;
   for (int y = 0; y < kH; ++y)
   {
      bad += px(0, y) != column_color(y % 8) ? 1 : 0;
      bad += px(3, y) != column_color((3 + y) % 8) ? 1 : 0;
   }
   CHECK(bad == 0);
   // 32-bit: x scroll in the low half, y in the high half (BG0's tile rows are all alike: only x shows)
   for (int y = 0; y < video::kLineEntries; ++y)
   {
      g_lines32[y] = static_cast<uint32_t>(7 - y % 8) | (uint32_t{static_cast<uint16_t>(y)} << 16);
   }
   video::set_line_effect(video::LineRegister::Bg0Hofs, video::LineWidth::Word, g_lines32);
   show();
   bad = 0;
   for (int y = 0; y < kH; ++y)
   {
      bad += px(0, y) != column_color(7 - y % 8) ? 1 : 0;
   }
   CHECK(bad == 0);
   video::set_line_effect(video::LineRegister::Bg0Hofs, video::LineWidth::Half, nullptr);
   video::set_bg_scroll(0, 2, 0);
   show();
   CHECK(px(0, 0) == column_color(2) && px(0, 100) == column_color(2));
}

/// Layer control per line (BG0 under BG1 on odd lines), blend weights per line, BLDCNT + BLDALPHA in 32 bits.
void scanline_control_and_blend()
{
   fresh();
   two_layers();
   const uint16_t front = 0 | (30 << 8);       // priority 0, screen block 30
   const uint16_t back = 2 | (30 << 8);        // priority 2: under BG1 (1)
   for (int y = 0; y < video::kLineEntries; ++y)
   {
      g_lines16[y] = (y & 1) ? back : front;
   }
   video::set_line_effect(video::LineRegister::Bg0Cnt, video::LineWidth::Half, g_lines16);
   show();
   const video::Color solid = video::rgb5(18, 22, 4);
   CHECK(px(0, 0) == column_color(0) && px(0, 1) == solid && px(5, 2) == column_color(5) && px(5, 3) == solid);
   video::set_line_effect(video::LineRegister::Bg0Cnt, video::LineWidth::Half, nullptr);

   // alpha of BG0 over BG1: line y's weights (y % 17, 16 - y % 17)
   video::set_blend(video::BlendMode::Alpha, 1, 2);
   for (int y = 0; y < video::kLineEntries; ++y)
   {
      const int a = y % 17;
      g_lines16[y] = static_cast<uint16_t>(a | ((16 - a) << 8));
   }
   video::set_line_effect(video::LineRegister::BldAlpha, video::LineWidth::Half, g_lines16);
   show();
   int bad = 0;
   for (int y = 0; y < kH; ++y)
   {
      const int a = y % 17;
      const video::Color c = column_color(0);
      uint16_t want = 0;
      for (int s = video::kRedShift; s <= video::kBlueShift; s += video::kChannelBits)
      {
         int v = (((c >> s) & 31) * a + ((solid >> s) & 31) * (16 - a)) >> 4;
         want = static_cast<uint16_t>(want | ((v > 31 ? 31 : v) << s));
      }
      bad += px(0, y) != want ? 1 : 0;
   }
   CHECK(bad == 0);
   // BLDCNT and BLDALPHA in one 32-bit table: even lines blend 8/8, odd lines no effect
   for (int y = 0; y < video::kLineEntries; ++y)
   {
      const uint32_t cnt = (y & 1) ? 0u : (1u | (1u << 6) | (2u << 8));   // alpha, BG0 over BG1
      g_lines32[y] = cnt | (uint32_t{8 | (8 << 8)} << 16);
   }
   video::set_line_effect(video::LineRegister::BldCnt, video::LineWidth::Word, g_lines32);
   show();
   CHECK(px(0, 1) == column_color(0) && px(0, 0) != column_color(0));
}

alignas(4) uint16_t g_lines_irq[video::kLineEntries];   ///< T1: a second table.

/// T1: the second table: one value to two layers' x scroll per line, beside a first table on window 0's edges.
void scanline_second_table()
{
   fresh();
   two_layers();
   video::set_window_rect(0, 0, 0, 0, kH);
   video::set_window_masks(1, 0, 2, 0);       // inside: BG0; outside: BG1
   video::set_windows(true, false, false);
   constexpr int kRight = 120;                // window 0 covers x 0-119 on every line
   for (int y = 0; y < video::kLineEntries; ++y)
   {
      g_lines16[y] = static_cast<uint16_t>(kRight);
      g_lines_irq[y] = static_cast<uint16_t>(y % 8);
   }
   video::set_line_effect(video::LineRegister::Win0H, video::LineWidth::Half, g_lines16);
   const video::LineRegister regs[] = {video::LineRegister::Bg0Hofs, video::LineRegister::Bg1Hofs};
   video::set_line_effect_irq(regs, 2, g_lines_irq);
   show();
   int bad = 0;
   for (int y = 0; y < kH; ++y)
   {
      bad += px(0, y) != column_color(y % 8) ? 1 : 0;    // BG0 inside, scrolled by the second table
      bad += px(kRight + 1, y) != video::rgb5(18, 22, 4) ? 1 : 0;   // BG1 (solid) outside
   }
   CHECK(bad == 0);
   video::set_line_effect_irq(nullptr, 0, nullptr);
   video::set_line_effect(video::LineRegister::Win0H, video::LineWidth::Half, nullptr);
   video::set_windows(false, false, false);
   show();
   CHECK(px(0, 3) == column_color(0));
}

/// T1: a mosaic limited to one layer: BG0's columns in blocks of 4, the layers' order unchanged.
void mosaic_one_layer()
{
   fresh();
   two_layers();
   constexpr uint8_t kBlock = 4;
   video::set_mosaic(kBlock);
   video::set_mosaic_layers(1);   // BG0 only
   show();
   CHECK(px(0, 0) == column_color(0) && px(3, 2) == column_color(0) && px(4, 0) == column_color(4) &&
         px(7, 3) == column_color(4));
   video::set_mosaic_layers(video::kLayerAll);
   video::set_mosaic(0);
   show();
   CHECK(px(3, 0) == column_color(3));
}

/// Window 0's edges per line through the generic table: plain, wrapped (right left of left), clamped.
void scanline_windows()
{
   fresh();
   two_layers();
   video::set_window_rect(0, 0, 0, 0, kH);   // its rows: the whole screen; its edges come from the table
   video::set_window_masks(1, 0, 2, 0);       // inside: BG0; outside: BG1
   video::set_windows(true, false, false);
   for (int y = 0; y < video::kLineEntries; ++y)
   {
      g_lines16[y] = y < 50 ? static_cast<uint16_t>((10 << 8) | 20)     // 10..19
                     : y < 100 ? static_cast<uint16_t>((200 << 8) | 30)   // wraps: 200..239 and 0..29
                               : static_cast<uint16_t>((230 << 8) | 250);   // right clamped to 240
   }
   video::set_line_effect(video::LineRegister::Win0H, video::LineWidth::Half, g_lines16);
   show();
   const video::Color solid = video::rgb5(18, 22, 4);
   CHECK(px(9, 10) == solid && px(10, 10) == column_color(2) && px(19, 10) == column_color(3) && px(20, 10) == solid);
   CHECK(px(5, 60) == column_color(5) && px(29, 60) == column_color(5) && px(30, 60) == solid);
   CHECK(px(199, 60) == solid && px(200, 60) == column_color(0) && px(239, 60) == column_color(7));
   CHECK(px(229, 120) == solid && px(230, 120) == column_color(6) && px(239, 120) == column_color(7));
}

/// An affine layer's reference point written per line restarts the internal reference.
void scanline_affine_reference()
{
   fresh();
   two_layers();
   // BG2 affine in mode 1: 8bpp tile 0 of colour (column + 1)
   uint32_t tile8[16];
   for (int r = 0; r < 8; ++r)
   {
      tile8[r * 2] = 0x04030201u;
      tile8[r * 2 + 1] = 0x08070605u;
   }
   video::load_bg_tiles_8bpp(1, 0, tile8, 1);
   video::set_mode(video::kModeMixed);
   video::configure_bg(0, false, 0, 0, 30, false, 0);
   video::configure_bg(1, false, 1, 0, 31, false, 0);
   video::configure_bg(2, true, 0, 1, 28, true, 0);
   video::set_bg_wrap(2, true);
   for (int y = 0; y < 16; ++y)
   {
      for (int x = 0; x < 16; ++x)
      {
         video::write_affine_cell(2, x, y, 0);
      }
   }
   // pb = 256: each line moves the texture one pixel right; a table that writes x = 0 every line undoes that
   video::set_bg_affine(2, kQ8One, kQ8One, 0, kQ8One, 0, 0);
   show();
   CHECK(px(0, 0) == column_color(0) && px(0, 3) == column_color(3));
   for (int y = 0; y < video::kLineEntries; ++y)
   {
      g_lines32[y] = 0;
   }
   video::set_line_effect(video::LineRegister::Bg2X, video::LineWidth::Word, g_lines32);
   show();
   CHECK(px(0, 0) == column_color(0) && px(0, 3) == column_color(0) && px(2, 77) == column_color(2));
   // the low half only (16-bit): x = 2 pixels (2 << 8) each line
   for (int y = 0; y < video::kLineEntries; ++y)
   {
      g_lines16[y] = 2 << kQ8Shift;
   }
   video::set_line_effect(video::LineRegister::Bg2X, video::LineWidth::Half, g_lines16);
   show();
   CHECK(px(0, 0) == column_color(2) && px(0, 50) == column_color(2));
}

/// The scanline module: two tables in turn, the wave's samples and movement.
void scanline_module()
{
   fresh();
   two_layers();
   namespace sl = screen::scanline;
   sl::start(video::LineRegister::Bg0Hofs, video::LineWidth::Half);
   uint16_t* a = sl::back16();
   for (int y = 0; y < sl::kEntries; ++y)
   {
      a[y] = 1;
   }
   sl::present();
   uint16_t* b = sl::back16();
   CHECK(a != b && sl::shown16() == a);
   for (int y = 0; y < sl::kEntries; ++y)
   {
      b[y] = 3;
   }
   show();   // the frame shows table a
   CHECK(px(0, 40) == column_color(1));
   sl::present();
   show();
   CHECK(px(0, 40) == column_color(3) && sl::back16() == a);
   sl::stop();
   show();
   CHECK(px(0, 40) == column_color(0) && !sl::active());

   // the wave: samples (sine * amplitude) / 256 from a game's sine table
   static int16_t sine[256];
   for (int i = 0; i < 256; ++i)
   {
      sine[i] = static_cast<int16_t>(sin_q14(static_cast<uint16_t>(i << 8)) >> 6);
   }
   sl::clear();
   tasks::reset();
   sl::start(video::LineRegister::Bg0Hofs, video::LineWidth::Half);
   const sl::Wave w = {sine, 0, 160, 4, 8, 1};
   const uint8_t task = sl::start_wave(w);
   CHECK(task != sl::kNoWave);
   auto sample = [&](int i) { return static_cast<uint16_t>((sine[(i * 4) & 255] * 8) / 256); };
   CHECK(sl::back16()[5] == sample(5) && sl::shown16()[5] == sample(5));
   sl::set_wave_base(100);
   tasks::run();   // writes offset 0 + base, waits (delay 1)
   CHECK(sl::back16()[5] == static_cast<uint16_t>(sample(5) + 100));
   sl::present();
   tasks::run();   // offset 0 again, then the start moves
   CHECK(sl::back16()[5] == static_cast<uint16_t>(sample(5) + 100));
   sl::present();
   tasks::run();   // offset 1
   CHECK(sl::back16()[5] == static_cast<uint16_t>(sample(6) + 100));
   sl::stop();
   CHECK(!tasks::active(task));
}

// --- palette blends -----------------------------------------------------------------------------------------

/// Blends of ranges, banks and tags, the unfaded form and a ramp.
void blends()
{
   fresh();
   video::Color c[16];
   for (int i = 0; i < 16; ++i)
   {
      c[i] = video::rgb5(static_cast<uint8_t>(i), 31, 0);
   }
   gfx::palette::load_obj(2 * 16, c, 16);
   gfx::palette::load_bg(16, c, 16);
   const uint8_t bank = sp::load_palette(77, c);   // first free bank: 0
   CHECK(bank == 0);
   CHECK(screen::blend::tag_mask(77) == gfx::palette::obj_bank(0) && screen::blend::tag_mask(78) == 0);
   CHECK(screen::blend::tag(77, 8, 0));
   CHECK(gfx::palette::visible(true, 5) == gfx::palette::blend_floor(c[5], 0, 8));
   CHECK(gfx::palette::base(true, 5) == c[5]);
   screen::blend::banks(gfx::palette::bg_bank(1) | gfx::palette::obj_bank(2), 16, video::rgb5(31, 31, 31));
   CHECK(gfx::palette::visible(false, 16 + 3) == video::rgb5(31, 31, 31));
   CHECK(gfx::palette::visible(true, 32 + 3) == video::rgb5(31, 31, 31));
   screen::blend::range(true, 33, 1, 4, 0);
   CHECK(gfx::palette::visible(true, 33) == gfx::palette::blend_floor(c[1], 0, 4));
   CHECK(gfx::palette::visible(true, 34) == video::rgb5(31, 31, 31));
   screen::blend::banks_unfaded(gfx::palette::obj_bank(2), 2, 0);
   CHECK(gfx::palette::visible(false, 19) == c[3] && gfx::palette::visible(true, 5) == c[5]);
   CHECK(gfx::palette::visible(true, 35) == gfx::palette::blend_floor(c[3], 0, 2));
   hal::vram::flush_all();
   CHECK(hal::soft::peek16(video::Region::ObjPalette, 35 * 2) == gfx::palette::blend_floor(c[3], 0, 2));

   // a ramp 0 -> 3 holding each amount 2 frames: shown 0, -, 1, -, 2, -, 3 then done
   screen::blend::Ramp r = {gfx::palette::obj_bank(2), 0, 0, 3, 1, 0};
   uint8_t shown[8];
   int n = 0;
   bool going = true;
   while (going && n < 8)
   {
      going = screen::blend::step(r);
      shown[n++] = r.coef;
   }
   CHECK(n == 7 && !going && gfx::palette::visible(true, 33) == gfx::palette::blend_floor(c[1], 0, 3));
   CHECK(shown[0] == 1 && shown[1] == 1 && shown[2] == 2 && shown[4] == 3);

   // a fade by tag
   const uint16_t tags[] = {77};
   CHECK(screen::blend::fade_tags(tags, 1, gfx::palette::bg_bank(1), 0, 0, 16, 0));
   CHECK(screen::fade::active());
}

// --- the bank and the player --------------------------------------------------------------------------------

anim::Bank g_bank;           ///< The test bank.
anim::Player g_player;       ///< The test player.

/// One log entry of the mark command.
struct Mark
{
   uint32_t frame;   ///< The player's frame.
   int16_t value;    ///< The command's value.
   int16_t arg0;     ///< Argument 0 then.
};
Mark g_marks[16];    ///< The log.
int g_mark_count = 0;   ///< Entries.

/// The test's game command 0x40: logs.
/// @param p  the player
void cmd_mark(anim::Player& p)
{
   ++p.pc;
   const int16_t v = anim::read_s16(p);
   if (g_mark_count < 16)
   {
      g_marks[g_mark_count++] = Mark{p.frames, v, p.args[0]};
   }
}

/// A visual task: counts argument 0's frames down, then ends.
/// @param id  the task
void count_down(uint8_t id)
{
   int16_t* d = tasks::task(id).data;
   if (d[1] == 0)
   {
      d[0] = g_player.args[0];
      d[1] = 1;
      return;
   }
   if (--d[0] == 0)
   {
      anim::finish_task(g_player, id);
   }
}

/// A sound task: the same, as a sound task.
/// @param id  the task
void beep(uint8_t id)
{
   int16_t* d = tasks::task(id).data;
   if (d[1] == 0)
   {
      d[0] = g_player.args[0];
      d[1] = 1;
      return;
   }
   if (--d[0] == 0)
   {
      anim::finish_sound_task(g_player, id);
   }
}

/// The square's callback: ends after 4 calls.
/// @param s  the sprite
void square_cb(sp::Sprite& s)
{
   if (++s.data[0] >= 4)
   {
      anim::finish_sprite(g_player, static_cast<uint8_t>(&s - &sp::sprite(0)));
   }
}

/// The spinner's callback: ends once its affine animation ended.
/// @param s  the sprite
void spinner_cb(sp::Sprite& s)
{
   if (s.affine_ended)
   {
      anim::finish_sprite(g_player, static_cast<uint8_t>(&s - &sp::sprite(0)));
   }
}

const sp::Template kSquareT = {1,          sp::kNoTag, video::OBJ_SQUARE, 1,      1, 0, sp::kAffineOff,
                               video::ObjMode::Normal, 0, nullptr,       nullptr, square_cb};   ///< The square.
const sp::Template kSpinnerT = {1,          sp::kNoTag, video::OBJ_SQUARE, 1,      1, 0, sp::kAffineNormal,
                                video::ObjMode::Normal, 0, nullptr,       nullptr, spinner_cb};   ///< The spinner.
/// The spinner's affine animation: two frames, End.
const af::Cmd kTurn[] = {af::frame(0, 0, 16, 2), af::end()};
/// Its table.
const af::Cmd* const kTurnTable[] = {kTurn};
const anim::SpriteTemplate kTemplates[] = {{&kSquareT, nullptr}, {&kSpinnerT, kTurnTable}};   ///< Templates.
const tasks::Func kTasks[] = {count_down};    ///< Visual tasks.
const tasks::Func kSoundTasks[] = {beep};     ///< Sound tasks.
const char* const kTemplateNames[] = {"square", "spinner"};   ///< Their names.
const char* const kTaskNames[] = {"count_down"};               ///< Their names.
const char* const kSoundTaskNames[] = {"beep"};                ///< Their names.
/// The tables (with names: bind() can use them).
const anim::Tables kTables = {kTemplates, 2,       kTasks, 1,       kSoundTasks, 1, nullptr, 0, nullptr, 0,
                              {kTemplateNames, kTaskNames, kSoundTaskNames, nullptr, nullptr}};

uint16_t g_last_se = 0;   ///< play_se's sound.
int8_t g_last_pan = 0;    ///< Its pan.
uint16_t g_loaded = 0;    ///< load_gfx's tag.
uint16_t g_unloaded = 0;  ///< unload_gfx's tag.

/// The test's hooks.
const anim::Hooks kHooks = {nullptr, nullptr, nullptr,
                            [](anim::Player&, uint16_t se, int8_t pan)
                            {
                               g_last_se = se;
                               g_last_pan = pan;
                            },
                            [](anim::Player&, uint16_t tag) { g_loaded = tag; },
                            [](anim::Player&, uint16_t tag) { g_unloaded = tag; }};
/// The test's command table: the generic ones and the mark command.
constexpr anim::CommandTable kCommands = anim::with(anim::generic_table(), anim::kFirstGameOpcode, cmd_mark);
/// The test's game.
const anim::Game kGame = {kCommands.at, &kTables, &kHooks};

/// Runs frames: the player, then the tasks, then the sprites (a scene's order).
/// @param n  frames
void run_frames(int n)
{
   for (int i = 0; i < n; ++i)
   {
      anim::frame(g_player);
      tasks::run();
      sp::animate();
   }
}

/// Starts a script by name.
/// @param name  its name
void play(const char* name)
{
   fresh();
   g_mark_count = 0;
   make_square_tiles();
   sp::load_sheet_words(1, g_square_tiles, 16);   // the templates' sheet
   const uint16_t s = anim::find_script(g_bank, name);
   CHECK(s < g_bank.script_count);
   anim::start(g_player, g_bank, kGame, s);
}

/// The bank's header and tables, and its code check.
void bank_reader()
{
   CHECK(anim::open(kTestAnimBank, kTestAnimBank_size, g_bank) == anim::BankError::None);
   CHECK((g_bank.flags & anim::kFlagIndexed) != 0);
   CHECK(g_bank.script_count == 9 && g_bank.symbol_count == 4 && g_bank.opcode_count == 2);
   static uint32_t bits[anim::boundary_words(1024)];
   CHECK(g_bank.code_size <= 1024);
   uint32_t bad = 0;
   CHECK(anim::check_code(g_bank, bits, bad));
   CHECK(anim::check_code(g_bank, nullptr, bad));
   CHECK(anim::symbol_kind(g_bank, 2) == anim::SymbolKind::Task && anim::symbol_index(g_bank, 1) == 1);
   const char* sig = anim::signature(g_bank, anim::kFirstGameOpcode);
   CHECK(sig != nullptr && sig[0] == 's' && sig[1] == '\0');
   CHECK(anim::signature(g_bank, 0x60) == nullptr && anim::signature(g_bank, 0x30) == nullptr);
   CHECK(anim::find_script(g_bank, "nothing") == g_bank.script_count);
   // a damaged copy: bad magic, short, a symbol of the wrong kind in the code
   static uint8_t copy[4096];
   CHECK(kTestAnimBank_size <= sizeof(copy));
   for (uint32_t i = 0; i < kTestAnimBank_size; ++i)
   {
      copy[i] = kTestAnimBank[i];
   }
   anim::Bank b2{};
   copy[0] ^= 1;
   CHECK(anim::open(copy, kTestAnimBank_size, b2) == anim::BankError::BadMagic);
   copy[0] ^= 1;
   CHECK(anim::open(copy, anim::kHeaderBytes - 1, b2) == anim::BankError::TooSmall);
   CHECK(anim::open(copy, kTestAnimBank_size, b2) == anim::BankError::None);
   const uint32_t visual = anim::script_address(b2, anim::find_script(b2, "visual"));
   uint8_t* code = const_cast<uint8_t*>(b2.code);
   code[visual + 1] = 2;   // create_sprite's template operand -> symbol 2, a task
   CHECK(!anim::check_code(b2, nullptr, bad) && bad == visual + 1);
}

/// The player's frames and commands.
void player()
{
   CHECK(anim::open(kTestAnimBank, kTestAnimBank_size, g_bank) == anim::BankError::None);

   // waits: delay 0 takes 2 frames, delay n n + 2, load_gfx 3
   play("delays");
   run_frames(20);
   CHECK(g_mark_count == 4 && g_marks[0].frame == 1 && g_marks[1].frame == 3 && g_marks[2].frame == 8 &&
         g_marks[3].frame == 11);
   CHECK(!anim::running(g_player) && g_player.error == anim::PlayerError::None && g_loaded == 7);

   // visual and sound counts
   play("visual");
   run_frames(1);
   CHECK(g_player.visual_count == 2 && sp::count() == 1 && sp::sprite(0).x == 0);
   run_frames(10);
   CHECK(g_mark_count == 3 && g_marks[0].frame == 1 && g_marks[1].frame == 4 && g_marks[2].frame == 6);
   CHECK(!anim::running(g_player) && sp::count() == 0 && tasks::count() == 0);
   CHECK(g_player.args[0] == 2 && g_player.args[1] == 20);   // args past a list's count keep their values

   // calls and arguments, all in one frame
   play("control");
   run_frames(1);
   CHECK(g_mark_count == 2 && g_marks[0].value == 9 && g_marks[0].arg0 == 6 && g_marks[1].value == 6);
   CHECK(!anim::running(g_player) && g_player.depth == 0);

   play("args_keep");
   run_frames(1);
   CHECK(g_player.args[0] == 4 && g_player.args[1] == 2 && g_player.args[2] == 3);

   // errors
   play("runaway");
   run_frames(1);
   CHECK(g_player.error == anim::PlayerError::Runaway && !anim::running(g_player));
   play("deep");
   run_frames(1);
   CHECK(g_player.error == anim::PlayerError::CallDepth);
   play("bad_opcode");
   run_frames(1);
   CHECK(g_player.error == anim::PlayerError::BadOpcode);
   anim::start(g_player, g_bank, kGame, g_bank.script_count);
   CHECK(g_player.error == anim::PlayerError::BadScript && !anim::running(g_player));

   // registers and hooks
   play("registers");
   screen::layers::init(screen::layers::LayerTemplate{1, 1, 29, 0, 0, 0, 0});
   run_frames(1);
   CHECK(g_mark_count == 1 && g_last_se == 12 && g_last_pan == -64 && g_unloaded == 7);
   CHECK(video::bg_scroll_x(1) == -3 && video::bg_scroll_y(1) == 4);
   CHECK((hal::soft::pending_bldcnt() >> 6 & 3) == 0);   // blend_off ran last
   CHECK(!anim::running(g_player));

   // an affine sprite made by a script: its matrix, its animation in the sprite step, its end
   play("affine_sprite");
   run_frames(1);
   const sp::Sprite& s = sp::sprite(0);
   // begun at creation (the turn's first frame), then the frame's sprite step (its second)
   CHECK(s.in_use && af::taken(s.matrix) && af::state(s.matrix).rotation == 0x2000);
   run_frames(1);   // End
   CHECK(sp::sprite(0).affine_ended);
   run_frames(3);   // its callback ends it next frame; then wait_visual passes
   CHECK(!sp::sprite(0).in_use && !af::taken(0) && !anim::running(g_player));

   // binding by name: the same results from a map
   static uint16_t map[16];
   CHECK(anim::bind(g_bank, kTables, map) == 0 && map[0] == 0 && map[1] == 1 && map[2] == 0 && map[3] == 0);
   fresh();
   g_mark_count = 0;
   anim::start(g_player, g_bank, kGame, anim::find_script(g_bank, "delays"), map);
   run_frames(20);
   CHECK(g_mark_count == 4 && g_marks[3].frame == 11);
}

}   // namespace

int main()
{
   affine_commands();
   affine_pixels();
   scanline_scroll();
   scanline_control_and_blend();
   scanline_windows();
   scanline_second_table();
   mosaic_one_layer();
   scanline_affine_reference();
   scanline_module();
   blends();
   bank_reader();
   player();
   std::printf("anim tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
