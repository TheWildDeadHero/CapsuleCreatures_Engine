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
   std::printf("soft video tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
