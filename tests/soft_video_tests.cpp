// Runtime tests for the software GBA video model (src/hal/soft), shared by the
// desktop, Dreamcast and headless HALs. Pure logic: no window, no SDL. Run on
// a big-endian host too (P-19): every check states GBA byte layout, so a
// host-order read anywhere in the emulation fails here.
#include "capsule/hal/video.h"

#include "../src/hal/soft/soft_video.h"

#include <cstdio>
#include <cstdlib>

namespace capsule::hal::system
{
[[noreturn]] void assert_failed(const char* expr, const char* file, int line)
{
   std::printf("ASSERT %s:%d: %s\n", file, line, expr);
   std::abort();
}
} // namespace capsule::hal::system

namespace
{

using namespace capsule;
using namespace capsule::hal;

int g_failures = 0;
int g_checks = 0;

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

uint16_t g_frame[soft::kFramePixels];
uint16_t pixel(int x, int y)
{
   return g_frame[y * video::kScreenWidth + x];
}

void Show()
{
   soft::latch();
   soft::compose(g_frame);
}

void ForcedBlankUntilFirstFrame()
{
   soft::reset();
   soft::compose(g_frame);   // not latched yet
   CHECK(pixel(0, 0) == 0x7FFF && pixel(239, 159) == 0x7FFF);
}

// One 4bpp tile row is a 32-bit word; the leftmost pixel is the lowest nibble.
void TileNibbleOrder()
{
   soft::reset();
   video::Color pal[32];
   for (int i = 0; i < 32; ++i)
   {
      pal[i] = static_cast<video::Color>(0x100 + i);
   }
   video::load_bg_palette(0, pal, 32);

   uint32_t tile[8];
   for (uint32_t& row : tile)
   {
      row = 0x76543210u;
   }   // pixels 0..7 use colors 0..7
   video::load_bg_tiles(0, 1, tile, 1);
   video::configure_bg(0, true, 0, 0, 31, false, 0);
   video::write_map_cell(0, 0, 0, video::map_cell(1, 1));   // palette bank 1
   video::set_mode(0);
   Show();

   CHECK(pixel(0, 0) == pal[0]);         // nibble 0 is transparent -> backdrop (color 0)
   for (int x = 1; x < 8; ++x)
   {
      CHECK(pixel(x, 0) == pal[16 + x]);
   }
   CHECK(pixel(8, 0) == pal[0]);         // next cell is tile 0: empty
}

void FlipsAndScroll()
{
   soft::reset();
   video::Color pal[32];
   for (int i = 0; i < 32; ++i)
   {
      pal[i] = static_cast<video::Color>(0x200 + i);
   }
   video::load_bg_palette(0, pal, 32);
   uint32_t tile[8];
   for (int r = 0; r < 8; ++r)
   {
      tile[r] = 0x00000001u | (static_cast<uint32_t>(r + 1) << 28);
   }
   video::load_bg_tiles(0, 1, tile, 1);
   video::configure_bg(0, true, 0, 0, 31, false, 0);
   video::write_map_cell(0, 0, 0, video::map_cell(1, 0, true, true));   // h + v flip
   video::set_mode(0);
   Show();
   CHECK(pixel(7, 7) == pal[1]);         // row 0, pixel 0 lands bottom-right
   CHECK(pixel(0, 7) == pal[1]);         // row 0, pixel 7 (value 1) lands bottom-left
   CHECK(pixel(0, 0) == pal[8]);         // row 7, pixel 7 (value 8) lands top-left

   video::set_bg_scroll(0, 1, 0);
   Show();
   CHECK(pixel(6, 7) == pal[1]);         // scrolled one pixel left
}

void MapCellRoundTrip()
{
   soft::reset();
   video::configure_bg(0, true, 0, 0, 31, false, 0);
   video::write_map_cell(0, 3, 2, 0xA5C3);
   CHECK(video::read_map_cell(0, 3, 2) == 0xA5C3);
}

void PixelConversions()
{
   CHECK(soft::bgr555_to_argb8888(0x001F) == 0xFFFF0000u);
   CHECK(soft::bgr555_to_argb8888(0x7C00) == 0xFF0000FFu);
   CHECK(soft::bgr555_to_argb8888(0x7FFF) == 0xFFFFFFFFu);
   CHECK(soft::bgr555_to_rgb565(0x001F) == 0xF800);
   CHECK(soft::bgr555_to_rgb565(0x03E0) == 0x07E0);
   CHECK(soft::bgr555_to_rgb565(0x7C00) == 0x001F);
   CHECK(soft::bgr555_to_rgb565(0x7FFF) == 0xFFFF);
}

} // namespace

// --- sprites, affine layers, color effects, windows -------------------------

constexpr video::Color kRed = video::rgb5(31, 0, 0);
constexpr video::Color kGreen = video::rgb5(0, 31, 0);
constexpr video::Color kBlue = video::rgb5(0, 0, 31);
constexpr video::Color kGrey = video::rgb5(10, 10, 10);

// BG0: tile 1 = solid color 1 in bank 0 everywhere; backdrop grey.
void SolidBg0(video::Color color, uint8_t priority = 0)
{
   soft::reset();
   const video::Color pal[2] = {kGrey, color};
   video::load_bg_palette(0, pal, 2);
   uint32_t solid[8];
   for (uint32_t& r : solid)
   {
      r = 0x11111111u;
   }
   video::load_bg_tiles(0, 1, solid, 1);
   video::configure_bg(0, true, priority, 0, 31, false, 0);
   for (int y = 0; y < 32; ++y)
   {
      for (int x = 0; x < 32; ++x)
      {
         video::write_map_cell(0, x, y, video::map_cell(1, 0));
      }
   }
   video::set_mode(0);
}

// One 8x8 sprite tile: left half color 1, right half color 2.
void LoadHalfTile()
{
   uint32_t t[8];
   for (uint32_t& r : t)
   {
      r = 0x22221111u;
   }
   video::load_obj_tiles(0, t, 1);
   const video::Color pal[3] = {0, kGreen, kBlue};
   video::load_obj_palette(16, pal, 3);   // OBJ bank 1
}

video::ObjDesc Obj8(int16_t x, int16_t y)
{
   video::ObjDesc d{};
   d.x = x;
   d.y = y;
   d.shape = video::OBJ_SQUARE;
   d.palette_bank = 1;
   return d;
}

void SpritesDrawFlipAndHide()
{
   SolidBg0(kRed, 1);
   LoadHalfTile();
   video::set_obj_layer(true);
   video::set_obj(0, video::make_obj(Obj8(10, 20)));
   video::ObjDesc f = Obj8(30, 20);
   f.hflip = true;
   video::set_obj(1, video::make_obj(f));
   Show();
   CHECK(pixel(10, 20) == kGreen && pixel(17, 27) == kBlue);   // left green, right blue
   CHECK(pixel(30, 20) == kBlue && pixel(37, 20) == kGreen);   // mirrored
   CHECK(pixel(9, 20) == kRed && pixel(18, 20) == kRed);       // outside: the layer
   video::set_obj(0, video::kObjHidden);
   Show();
   CHECK(pixel(10, 20) == kRed);
   // Latched: a change shows only after the next latch.
   video::set_obj(0, video::make_obj(Obj8(10, 20)));
   soft::compose(g_frame);
   CHECK(pixel(10, 20) == kRed);
}

void SpritePriorityAgainstLayers()
{
   SolidBg0(kRed, 0);   // layer in front (priority 0)
   LoadHalfTile();
   video::set_obj_layer(true);
   video::ObjDesc d = Obj8(10, 20);
   d.priority = 1;
   video::set_obj(0, video::make_obj(d));
   Show();
   CHECK(pixel(10, 20) == kRed);     // behind BG0
   d.priority = 0;
   video::set_obj(0, video::make_obj(d));
   Show();
   CHECK(pixel(10, 20) == kGreen);   // equal priority: sprite in front
}

void AffineSpriteDoubleSize()
{
   SolidBg0(kRed, 1);
   LoadHalfTile();
   video::set_obj_layer(true);
   video::ObjDesc d = Obj8(40, 40);
   d.affine = true;
   d.double_size = true;
   video::set_obj(0, video::make_obj(d));
   video::set_obj_affine(0, 128, 0, 0, 128);   // texture step 1/2: twice as big
   Show();
   // 16x16 box at (40, 40): left 8 columns green, right 8 blue.
   CHECK(pixel(40, 40) == kGreen && pixel(47, 55) == kGreen);
   CHECK(pixel(48, 40) == kBlue && pixel(55, 55) == kBlue);
   CHECK(pixel(56, 48) == kRed && pixel(39, 48) == kRed);
}

void AffineBackgroundIdentityAndWrap()
{
   soft::reset();
   const video::Color pal[3] = {kGrey, kRed, kGreen};
   video::load_bg_palette(0, pal, 3);
   uint32_t t8[16];
   for (int i = 0; i < 16; ++i)
   {
      t8[i] = 0x01010101u;
   }   // tile 1: color 1
   video::load_bg_tiles_8bpp(2, 1, t8, 1);
   for (int i = 0; i < 16; ++i)
   {
      t8[i] = 0x02020202u;
   }   // tile 2: color 2
   video::load_bg_tiles_8bpp(2, 2, t8, 1);
   video::set_mode(1);
   video::configure_bg(2, true, 0, 2, 30, true, 0);        // 128x128
   for (int y = 0; y < 16; ++y)
   {
      for (int x = 0; x < 16; ++x)
      {
         video::write_affine_cell(2, x, y, static_cast<uint8_t>(x == 0 ? 2 : 1));
      }
   }
   video::set_bg_affine(2, 256, 0, 0, 256, 0, 0);
   Show();
   CHECK(pixel(0, 0) == kGreen && pixel(8, 0) == kRed);
   CHECK(pixel(130, 10) == kGrey);                          // outside, no wrap: backdrop
   video::set_bg_wrap(2, true);
   Show();
   CHECK(pixel(130, 10) == kGreen);                         // x 130 wraps to 2
   video::set_bg_affine(2, 256, 0, 0, 256, 8 << 8, 0);      // reference point moves the map left
   Show();
   CHECK(pixel(0, 0) == kRed && pixel(120, 0) == kGreen);
}

void AlphaBrightnessAndSemiTransparent()
{
   SolidBg0(video::rgb5(16, 0, 0), 1);
   LoadHalfTile();
   video::set_obj_layer(true);
   video::set_obj(0, video::make_obj(Obj8(10, 20)));
   video::set_blend(video::BlendMode::Alpha, video::kLayerObj, 0x01);
   video::set_blend_alpha(8, 8);
   Show();
   CHECK(pixel(10, 20) == video::rgb5(8, 15, 0));    // half green + half red
   CHECK(pixel(50, 50) == video::rgb5(16, 0, 0));    // layer alone: no second target
   video::set_blend(video::BlendMode::Darken, 0x01, 0);
   video::set_blend_brightness(16);
   Show();
   CHECK(pixel(50, 50) == 0 && pixel(10, 20) == kGreen);   // only BG0 darkened
   video::set_blend(video::BlendMode::Brighten, 0x01, 0x01);
   video::set_blend_brightness(8);
   video::ObjDesc semi = Obj8(10, 20);
   semi.mode = video::ObjMode::SemiTransparent;
   video::set_obj(0, video::make_obj(semi));
   Show();
   CHECK(pixel(10, 20) == video::rgb5(8, 15, 0));    // semi-transparent sprite blends anyway
   CHECK(pixel(50, 50) == video::rgb5(23, 15, 15));  // BG0 brightened halfway
}

void WindowsMaskLayersAndEffects()
{
   SolidBg0(kRed, 0);
   video::set_window_rect(0, 10, 10, 20, 20);
   video::set_window_masks(0x01 | video::kWindowEffects, 0, 0, 0);
   video::set_windows(true, false, false);
   Show();
   CHECK(pixel(10, 10) == kRed && pixel(19, 19) == kRed);
   CHECK(pixel(20, 10) == kGrey && pixel(5, 5) == kGrey);   // outside shows nothing but backdrop
   video::set_blend(video::BlendMode::Darken, 0x01, 0);
   video::set_blend_brightness(16);
   video::set_window_masks(0x01, 0, 0x01 | video::kWindowEffects, 0);
   Show();
   CHECK(pixel(15, 15) == kRed && pixel(50, 50) == 0);      // effects only outside
   // OBJ window: a window-mode sprite's opaque pixels.
   LoadHalfTile();
   video::set_obj_layer(true);
   video::ObjDesc w = Obj8(100, 100);
   w.mode = video::ObjMode::Window;
   video::set_obj(0, video::make_obj(w));
   video::set_blend(video::BlendMode::Off, 0, 0);
   video::set_windows(false, false, true);
   video::set_window_masks(0, 0, 0, 0x01);
   Show();
   CHECK(pixel(100, 100) == kRed && pixel(50, 50) == kGrey);
}

/// @name FX2 (M5): the per-line OBJ budget and the frame composed at the end of the game's frame
///@{
constexpr int kBigSize = 3;             ///< Size 3 of a square: 64 x 64.
constexpr int kBigTiles = 64;           ///< Its 8 x 8 tiles (1D mapping).
constexpr int kStacked = 19;            ///< Normal 64-wide sprites the 1210-cycle budget draws on one line ...
constexpr int16_t kLoneX = 120;         ///< ... so a 20th, on its own columns, ...
constexpr int16_t kLoneY = 40;          ///< ... and lower, is dropped where it shares their lines.
constexpr int kAffineFits = 5;          ///< Affine double-size 64 x 64 sprites (8 + 2 x 128 cycles) a line draws.
constexpr int16_t kAffineFirstX = 48;   ///< Their boxes' columns: the first's left edge ...
constexpr int16_t kAffineStepX = 32;    ///< ... and the step.
constexpr int16_t kAffineLastX = -32;   ///< The sixth's box: left of the screen ...
constexpr int16_t kAffineLastY = 60;    ///< ... and lower.
constexpr int16_t kQ8Identity = 256;    ///< 1.0 in the affine parameters (Q8).
constexpr int kAffineProbeX = 10;       ///< A column only the sixth's picture covers ...
constexpr int kAffineSharedRow = 100;   ///< ... on a row the first five's boxes share ...
constexpr int kAffineOwnRow = 140;      ///< ... and on one below them.
constexpr int kLoneOwnRow = 64;         ///< The first row below the stack.
constexpr int kTableLine = 5;           ///< The scanline table's line written during the frame.
constexpr int16_t kTableScroll = 8;     ///< Its BG0 x scroll: the second tile column shows at x 0.

/// 64 solid tiles of colour 1 in OBJ VRAM (one 64 x 64 sprite), OBJ bank 1 colour 1 green.
void load_big_sprite_tiles()
{
   uint32_t t[kBigTiles * 8];
   for (uint32_t& r : t)
   {
      r = 0x11111111u;
   }
   video::load_obj_tiles(0, t, kBigTiles);
   const video::Color pal[2] = {0, kGreen};
   video::load_obj_palette(16, pal, 2);   // OBJ bank 1
}

/// A 64 x 64 sprite description.
/// @param x  left edge
/// @param y  top edge
/// @return it
video::ObjDesc big_obj(int16_t x, int16_t y)
{
   video::ObjDesc d = Obj8(x, y);
   d.size = kBigSize;
   return d;
}

/// The GBA's OBJ unit draws a line's sprites in OAM order until the line's 1210 cycles are spent (a normal sprite:
/// its width - 2, plus 2 per OAM entry stepped): a sprite past the budget is not drawn on that line.
void obj_line_budget_drops_late_sprites()
{
   SolidBg0(kRed, 1);
   load_big_sprite_tiles();
   video::set_obj_layer(true);
   for (int i = 0; i < kStacked; ++i)
   {
      video::set_obj(static_cast<uint8_t>(i), video::make_obj(big_obj(0, 0)));
   }
   video::set_obj(kStacked, video::make_obj(big_obj(kLoneX, kLoneY)));
   Show();
   CHECK(pixel(0, 0) == kGreen);                // the stack is drawn
   CHECK(pixel(kLoneX, kLoneY) == kRed);        // the 20th shares lines 40-63 with it: past the budget
   CHECK(pixel(kLoneX, kLoneOwnRow) == kGreen);  // below the stack it has the line to itself
   video::set_obj(kStacked - 1, video::kObjHidden);   // one fewer: the budget reaches the 20th
   Show();
   CHECK(pixel(kLoneX, kLoneY) == kGreen);
}

/// Affine sprites cost 8 + 2 cycles per pixel of their box: five double-size 64 x 64 ones fit a line, a sixth on the
/// same lines does not.
void obj_line_budget_affine()
{
   SolidBg0(kRed, 1);
   load_big_sprite_tiles();
   video::set_obj_layer(true);
   for (int i = 0; i < kAffineFits; ++i)
   {
      video::ObjDesc d = big_obj(static_cast<int16_t>(kAffineFirstX + i * kAffineStepX), 0);
      d.affine = true;
      d.double_size = true;
      video::set_obj(static_cast<uint8_t>(i), video::make_obj(d));
   }
   video::ObjDesc last = big_obj(kAffineLastX, kAffineLastY);   // its picture: columns 0-63, rows 92-155
   last.affine = true;
   last.double_size = true;
   video::set_obj(kAffineFits, video::make_obj(last));
   video::set_obj_affine(0, kQ8Identity, 0, 0, kQ8Identity);   // the 64 x 64 picture in the middle of its 128 box
   Show();
   CHECK(pixel(kAffineProbeX, kAffineSharedRow) == kRed);    // the first five's boxes span rows 0-127
   CHECK(pixel(kAffineProbeX, kAffineOwnRow) == kGreen);     // below them the sixth has the line to itself
   video::set_obj(kAffineFits - 1, video::kObjHidden);        // four before it: it fits
   Show();
   CHECK(pixel(kAffineProbeX, kAffineSharedRow) == kGreen);
}

/// present(): the latched frame is composed when the game's frame has run, so a scanline table written after the
/// latch shows in it (the GBA's HBlank transfer reads the table while the frame is drawn), while registers written
/// after the latch wait for the next one.
void present_reads_tables_written_during_the_frame()
{
   soft::reset();
   const video::Color pal[3] = {kGrey, kRed, kBlue};
   video::load_bg_palette(0, pal, 3);
   uint32_t tiles[2][8];
   for (uint32_t& r : tiles[0])
   {
      r = 0x11111111u;
   }
   for (uint32_t& r : tiles[1])
   {
      r = 0x22222222u;
   }
   video::load_bg_tiles(0, 1, tiles[0], 2);
   video::configure_bg(0, true, 0, 0, 31, false, 0);
   for (int y = 0; y < 32; ++y)
   {
      video::write_map_cell(0, 0, y, video::map_cell(1, 0));
      video::write_map_cell(0, 1, y, video::map_cell(2, 0));
   }
   video::set_mode(0);
   static uint16_t table[video::kLineEntries] = {};
   video::set_line_effect(video::LineRegister::Bg0Hofs, video::LineWidth::Half, table);
   soft::latch();
   table[kTableLine] = kTableScroll;    // the game's frame writes the table the display reads
   video::set_bg_scroll(0, 0, 1);       // and a register: it waits for the next latch
   const uint16_t* f = soft::present();
   CHECK(f[kTableLine * video::kScreenWidth] == kBlue);       // the table's write shows in this frame
   CHECK(f[(kTableLine + 1) * video::kScreenWidth] == kRed);  // the other lines keep their scroll
   CHECK(soft::shown() == f && soft::shown()[0] == kRed);    // no y scroll yet: row 0 is still row 0
   video::set_line_effect(video::LineRegister::Bg0Hofs, video::LineWidth::Half, nullptr);
}
///@}

/// @name FX3 (M5): the OBJ window's colour effects do not depend on OAM order
///@{
constexpr int16_t kObjWinX = 60;     ///< The window-mode sprite and the sprite under it ...
constexpr int16_t kObjWinY = 30;     ///< ... share this 8 x 8 square.
constexpr int16_t kOutsideX = 90;    ///< A second sprite, outside the OBJ window.
constexpr uint8_t kDarkenFull = 16;  ///< BLDY 16: a darkened pixel is black.
constexpr uint8_t kFirstEntry = 0;   ///< OAM entries of the test: the first ...
constexpr uint8_t kSecondEntry = 1;  ///< ... the second ...
constexpr uint8_t kThirdEntry = 2;   ///< ... and the third.
constexpr uint8_t kBg0Bit = 0x01;    ///< Blend / window mask bit of BG0.

/// The OBJ window decides each pixel's colour effects when the line is composed: a sprite's pixel inside it takes the
/// OBJ window's effects whether the window-mode sprite has a higher or a lower OAM number than the sprite (the
/// hardware's window logic reads the finished line's window flags; mGBA's software renderer instead bakes the
/// darken / brighten palette into sprites drawn before the window-mode sprite, an emulator artifact the Encore and
/// Flatter shots show, see FX3's REVIEW.md).
void obj_window_effects_do_not_depend_on_oam_order()
{
   for (int window_first = 0; window_first < 2; ++window_first)
   {
      SolidBg0(kRed, 1);
      LoadHalfTile();
      video::set_obj_layer(true);
      video::ObjDesc win = Obj8(kObjWinX, kObjWinY);
      win.mode = video::ObjMode::Window;
      const uint8_t win_entry = window_first != 0 ? kFirstEntry : kSecondEntry;
      const uint8_t obj_entry = window_first != 0 ? kSecondEntry : kFirstEntry;
      video::set_obj(win_entry, video::make_obj(win));
      video::set_obj(obj_entry, video::make_obj(Obj8(kObjWinX, kObjWinY)));
      video::set_obj(kThirdEntry, video::make_obj(Obj8(kOutsideX, kObjWinY)));
      // outside: every layer with effects; inside the OBJ window: every layer, no effects (the Encore set-up)
      video::set_window_masks(0, 0, video::kLayerAll, static_cast<uint8_t>(video::kLayerAll & ~video::kWindowEffects));
      video::set_windows(false, false, true);
      video::set_blend(video::BlendMode::Darken, static_cast<uint8_t>(kBg0Bit | video::kLayerObj), 0);
      video::set_blend_brightness(kDarkenFull);
      Show();
      CHECK(pixel(kObjWinX, kObjWinY) == kGreen);   // inside the OBJ window: not darkened, either order
      CHECK(pixel(kOutsideX, kObjWinY) == 0);       // outside: the sprite is darkened to black
      CHECK(pixel(kObjWinX - 1, kObjWinY) == 0);    // the layer outside too
   }
   video::set_blend(video::BlendMode::Off, 0, 0);
   video::set_windows(false, false, false);
}
///@}

int main()
{
   ForcedBlankUntilFirstFrame();
   TileNibbleOrder();
   FlipsAndScroll();
   MapCellRoundTrip();
   PixelConversions();
   SpritesDrawFlipAndHide();
   SpritePriorityAgainstLayers();
   AffineSpriteDoubleSize();
   AffineBackgroundIdentityAndWrap();
   AlphaBrightnessAndSemiTransparent();
   WindowsMaskLayersAndEffects();
   obj_line_budget_drops_late_sprites();
   obj_line_budget_affine();
   present_reads_tables_written_during_the_frame();
   obj_window_effects_do_not_depend_on_oam_order();
   std::printf("soft video tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
