/// @file
/// Dreamcast HAL (P-8) on KallistiOS (BSD-style KOS License: attribution only).
///
/// Video: the shared software GBA video model (src/hal/soft) composes each
/// 240x160 frame. It is stretched to fill the whole 640x480 screen (no border;
/// the 3:2 image is shown 4:3, so about 11% narrower than on a GBA) with "sharp
/// bilinear" scaling:
///   1. the CPU doubles the frame to 480x320 (each pixel becomes a 2x2 block)
///      into a PowerVR texture;
///   2. the PowerVR draws that texture as one full-screen quad with bilinear
///      filtering, scaling the remaining 4/3 x 3/2.
/// Pixels stay sharp and evenly sized; only their edges are blended, so there
/// is none of the uneven-column shimmer of a nearest-neighbour 8/3 stretch, and
/// none of the blur of filtering the 1x frame directly.
///
/// P-6: the PowerVR takes IEEE-754 single-precision vertex coordinates. Every
/// coordinate here is a constant that is exact in binary (640, 480, n/512), so
/// the HAL stores their bit patterns as integers, built by ieee_single_bits()
/// at compile time, in its own vertex struct. No floating-point type is used.
///
/// Pacing: one frame per vertical blank (60 Hz NTSC/VGA, 50 Hz PAL), not the
/// GBA's 59.73 Hz; pvr_wait_ready() waits for the vblank that starts a frame.
/// Input: first controller on the Maple bus. A/B/X/Y/Start/D-pad map directly,
/// analog triggers past half travel act as L/R; there is no Select (B-11
/// "dreamcast" input target).
#include "capsule/core/assert.h"
#include "capsule/core/random.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"

#include "../common/audio_hook.h"
#include "../soft/soft_video.h"

#include <cstdint>
#include <cstdio>

// KOS headers use GNU anonymous structs and partly initialized defaults
// (pvr_default_params); keep those warnings quiet for them only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <arch/arch.h>
#include <arch/timer.h>
#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/pvr.h>
#include <dc/video.h>
#pragma GCC diagnostic pop

namespace
{

using namespace capsule::hal;

constexpr int kFbWidth = 640;            ///< Framebuffer width.
constexpr int kFbHeight = 480;           ///< Framebuffer height.
constexpr int kTriggerThreshold = 128;   ///< Analog trigger travel (of 0-255) that counts as L / R.
constexpr int kHighWordShift = 32;       ///< Bits 32-63 of a 64-bit counter.
constexpr int kFrameEntropyShift = 16;   ///< Moves the frame count to the upper half of the entropy.
constexpr int kPvrLists = 5;             ///< PowerVR polygon lists (opb_sizes entries).
constexpr int kVertexBufBytes = 64 * 1024;        ///< PowerVR vertex buffer size.
constexpr std::size_t kTxrBlockBytes = 32;        ///< pvr_txr_load() copies whole blocks of this size.
constexpr int kTaVertexBytes = 32;                ///< One TA vertex.

/// @name IEEE-754 single precision
///@{
constexpr int kTopBit = 31;                        ///< Highest bit of a 32-bit integer.
constexpr int kFloatExponentBias = 127;            ///< Exponent bias.
constexpr int kFloatMantissaBits = 23;             ///< Stored mantissa bits (the leading 1 is implicit).
constexpr uint32_t kFloatMantissaMask = 0x7FFFFFu; ///< Stored mantissa field.
///@}

/// @name Assert screen
///@{
constexpr int kAssertChars = 96;   ///< Room for "file:line".
constexpr int kTextLeft = 24;      ///< Left edge of the message, framebuffer pixels.
constexpr int kTitleTop = 24;      ///< Top of the title line.
constexpr int kExprTop = 56;       ///< Top of the failed expression.
constexpr int kWhereTop = 88;      ///< Top of the file and line.
///@}

/// @name Texture
/// The 2x frame with a one-texel border on every side, in a 512x512 RGB565
/// non-twiddled texture (512 KB of the 8 MB video RAM). The border repeats
/// the edge pixels, so bilinear samples at the screen edges blend with a copy
/// of the edge instead of with unrelated texels.
///@{
constexpr int kPrescale = 2;                                ///< CPU prescale factor.
constexpr int kImageW = video::kScreenWidth * kPrescale;    ///< Prescaled width, 480.
constexpr int kImageH = video::kScreenHeight * kPrescale;   ///< Prescaled height, 320.
constexpr int kTexSize = 512;                               ///< Texture size; power of two, both axes.
constexpr int kTexRows = kImageH + 2;                       ///< Rows that are uploaded.
constexpr int kTexCols = kImageW + 2;                       ///< Columns written (image and its border).
constexpr std::size_t kUploadBytes =
   std::size_t{kTexSize} * kTexRows * sizeof(uint16_t);   ///< Bytes uploaded per frame.
///@}

static_assert(kImageW + 2 <= kTexSize && kImageH + 2 <= kTexSize, "image must fit the texture");
static_assert(kUploadBytes % kTxrBlockBytes == 0, "pvr_txr_load copies whole 32-byte blocks");

/// Bit pattern of the IEEE-754 single-precision value n / 2^k, for n < 2^24.
/// Every value used here is exact, so there is no rounding to get wrong.
/// @param n  numerator
/// @param k  power of two in the denominator
/// @return the 32-bit pattern
constexpr uint32_t ieee_single_bits(uint32_t n, int k)
{
   if (n == 0)
   {
      return 0;
   }
   int msb = kTopBit;
   while ((n >> msb) == 0)
   {
      --msb;
   }   // highest set bit: n = 1.xxx * 2^msb

   // Normalize: the leading 1 moves to bit 23 and is dropped from the stored mantissa.
   const uint32_t exponent = static_cast<uint32_t>(msb - k + kFloatExponentBias);
   const uint32_t mantissa = (n << (kFloatMantissaBits - msb)) & kFloatMantissaMask;
   return (exponent << kFloatMantissaBits) | mantissa;
}
static_assert(ieee_single_bits(1, 0) == 0x3F800000u, "1");
static_assert(ieee_single_bits(640, 0) == 0x44200000u, "640");
static_assert(ieee_single_bits(480, 0) == 0x43F00000u, "480");
static_assert(ieee_single_bits(1, 1) == 0x3F000000u, "0.5");
static_assert(ieee_single_bits(3, 2) == 0x3F400000u, "0.75");

/// Same layout as KOS's pvr_vertex_t (TA command, x, y, z, u, v, colour,
/// offset colour), with the coordinates held as their bit patterns.
struct alignas(kTaVertexBytes) Vertex
{
   uint32_t flags;   ///< TA command (PVR_CMD_VERTEX / _EOL).
   uint32_t x;       ///< Screen x, IEEE-754 bits.
   uint32_t y;       ///< Screen y, IEEE-754 bits.
   uint32_t z;       ///< Depth (1/w), IEEE-754 bits.
   uint32_t u;       ///< Texture u, IEEE-754 bits.
   uint32_t v;       ///< Texture v, IEEE-754 bits.
   uint32_t argb;    ///< Vertex colour (ignored: PVR_TXRENV_REPLACE).
   uint32_t oargb;   ///< Offset colour.
};
static_assert(sizeof(Vertex) == 32, "one TA vertex is 32 bytes");

/// @name Quad coordinates (IEEE-754 bit patterns)
///@{
constexpr uint32_t kX0 = ieee_single_bits(0, 0);                     ///< Left edge, 0.
constexpr uint32_t kX1 = ieee_single_bits(kFbWidth, 0);              ///< Right edge, 640.
constexpr uint32_t kY0 = ieee_single_bits(0, 0);                     ///< Top edge, 0.
constexpr uint32_t kY1 = ieee_single_bits(kFbHeight, 0);             ///< Bottom edge, 480.
constexpr uint32_t kZ = ieee_single_bits(1, 0);                     ///< Depth, 1.
constexpr int kTexShift = 9;                                         ///< 512 = 2^9.
static_assert((1 << kTexShift) == kTexSize, "texture size");
constexpr uint32_t kU0 = ieee_single_bits(1, kTexShift);             ///< Left texel edge, inside the border.
constexpr uint32_t kU1 = ieee_single_bits(1 + kImageW, kTexShift);   ///< Right texel edge.
constexpr uint32_t kV0 = ieee_single_bits(1, kTexShift);             ///< Top texel edge, inside the border.
constexpr uint32_t kV1 = ieee_single_bits(1 + kImageH, kTexShift);   ///< Bottom texel edge.
constexpr uint32_t kWhite = 0xFFFFFFFFu;                             ///< Opaque white.
///@}

/// Full-screen strip: top-left, top-right, bottom-left, bottom-right.
constexpr Vertex kQuad[4] = {
   {PVR_CMD_VERTEX, kX0, kY0, kZ, kU0, kV0, kWhite, 0},
   {PVR_CMD_VERTEX, kX1, kY0, kZ, kU1, kV0, kWhite, 0},
   {PVR_CMD_VERTEX, kX0, kY1, kZ, kU0, kV1, kWhite, 0},
   {PVR_CMD_VERTEX_EOL, kX1, kY1, kZ, kU1, kV1, kWhite, 0},
};

uint16_t g_frame555[soft::kFramePixels];             ///< Composed frame, BGR555.
alignas(kTxrBlockBytes) uint16_t g_upload[kTexSize * kTexRows];   ///< Texture rows, staged in main RAM.
pvr_ptr_t g_texture = nullptr;                        ///< The texture in video RAM.
pvr_poly_hdr_t g_header;                              ///< Compiled polygon header for the quad.
input::KeyBits g_held = 0;                            ///< Keys read at the last update().
uint32_t g_frames = 0;                                ///< Frames since init().

/// Composed frame -> 2x prescaled, edge-padded texture rows in g_upload.
void prescale()
{
   for (int y = 0; y < video::kScreenHeight; ++y)
   {
      const uint16_t* src = g_frame555 + y * video::kScreenWidth;
      uint16_t* row0 = g_upload + (1 + y * kPrescale) * kTexSize;
      uint16_t* row1 = row0 + kTexSize;
      for (int x = 0; x < video::kScreenWidth; ++x)
      {
         const uint16_t c = soft::bgr555_to_rgb565(src[x]);
         const int tx = 1 + x * kPrescale;
         row0[tx] = c;
         row0[tx + 1] = c;
         row1[tx] = c;
         row1[tx + 1] = c;
      }
   }
   // Border: repeat the outermost columns, then the outermost rows.
   for (int y = 1; y <= kImageH; ++y)
   {
      uint16_t* row = g_upload + y * kTexSize;
      row[0] = row[1];
      row[kImageW + 1] = row[kImageW];
   }
   for (int x = 0; x < kTexCols; ++x)
   {
      g_upload[x] = g_upload[kTexSize + x];
      g_upload[(kImageH + 1) * kTexSize + x] = g_upload[kImageH * kTexSize + x];
   }
}

/// Uploads the texture and draws the full-screen quad; paces to vblank.
void draw_frame()
{
   pvr_wait_ready();          // paces to vblank
   pvr_wait_render_done();    // the previous frame no longer reads the texture
   pvr_txr_load(g_upload, g_texture, kUploadBytes);
   pvr_scene_begin();
   pvr_list_begin(PVR_LIST_OP_POLY);
   pvr_prim(&g_header, sizeof g_header);
   for (const Vertex& v : kQuad)
   {
      pvr_prim(&v, sizeof v);
   }
   pvr_list_finish();
   pvr_scene_finish();
}

/// Initializes the PowerVR (opaque list only), allocates the texture and
/// compiles the quad's polygon header.
void init_pvr()
{
   pvr_init_params_t params = pvr_default_params;
   // Opaque polygons only; one quad needs very little vertex space.
   params.opb_sizes[0] = PVR_BINSIZE_16;
   for (int i = 1; i < kPvrLists; ++i)
   {
      params.opb_sizes[i] = PVR_BINSIZE_0;
   }
   params.vertex_buf_size = kVertexBufBytes;
   pvr_init(&params);

   g_texture = pvr_mem_malloc(std::size_t{kTexSize} * kTexSize * sizeof(uint16_t));
   if (g_texture == nullptr)
   {   // release builds too: without the texture nothing can be shown, so halt on the assert screen
      system::assert_failed("pvr_mem_malloc: no PowerVR memory for the frame texture", __FILE__, __LINE__);
   }

   pvr_poly_cxt_t cxt;
   pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, kTexSize, kTexSize, g_texture,
                    PVR_FILTER_BILINEAR);
   cxt.gen.culling = PVR_CULLING_NONE;
   cxt.txr.env = PVR_TXRENV_REPLACE;    // texture colour only, vertex colour ignored
   pvr_poly_compile(&g_header, &cxt);
}

/// Reads the first Maple controller.
/// @return held keys; 0 when no controller is attached
input::KeyBits read_controller()
{
   maple_device_t* dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
   if (dev == nullptr)
   {
      return 0;
   }
   const auto* st = static_cast<const cont_state_t*>(maple_dev_status(dev));
   if (st == nullptr)
   {
      return 0;
   }
   /// One controller button.
   struct Bit
   {
      uint32_t mask;    ///< CONT_* bit.
      input::Key key;   ///< Engine button.
   };
   constexpr Bit kBits[] = {
      {CONT_A, input::KEY_A},
      {CONT_B, input::KEY_B},
      {CONT_X, input::KEY_X},
      {CONT_Y, input::KEY_Y},
      {CONT_START, input::KEY_START},
      {CONT_DPAD_UP, input::KEY_UP},
      {CONT_DPAD_DOWN, input::KEY_DOWN},
      {CONT_DPAD_LEFT, input::KEY_LEFT},
      {CONT_DPAD_RIGHT, input::KEY_RIGHT},
   };
   input::KeyBits held = 0;
   for (const Bit& b : kBits)
   {
      if (st->buttons & b.mask)
      {
         held |= input::key_bit(b.key);
      }
   }
   if (st->ltrig >= kTriggerThreshold)
   {
      held |= input::key_bit(input::KEY_L);
   }
   if (st->rtrig >= kTriggerThreshold)
   {
      held |= input::key_bit(input::KEY_R);
   }
   return held;
}

} // namespace

namespace capsule::hal::system
{

void init()
{
   vid_set_mode(DM_640x480, PM_RGB565);   // pvr_init() expects a 2D mode first
   vid_border_color(0, 0, 0);
   init_pvr();
   soft::reset();
   vram::reset();
   rng::set_blank_hook(audio::hook::spend);   // C14a (D-34): modelled lag blanks step the sound too
   rng::seed(entropy());
}

uint32_t entropy()
{
   const uint64_t us = timer_us_gettime64();
   return static_cast<uint32_t>(us) ^ static_cast<uint32_t>(us >> kHighWordShift) ^ (g_frames << kFrameEntropyShift);
}

bool update()
{
   vram::flush();   // A-5: queued uploads land at "VBlank", with the registers
   soft::latch();
   soft::compose(g_frame555);
   prescale();
   draw_frame();
   audio::hook::vblank();   // M4: the audio step, once per blank (nothing until audio starts)
   g_held = read_controller();
   ++g_frames;
   rng::frame_advance();   // N1: a game's per-frame draw at the vertical blank (off by default)
   seed_on_first_press(g_held);
   return true;
}

// Shown on screen with the BIOS font, then halts, as the GBA HAL does. Also
// printed, which reaches a dcload console when one is attached. The PowerVR is
// shut down first so the plain framebuffer is on screen again.
uint32_t vblanks()
{
   return g_frames;   // C11: one blank per update() (the frames are paced: no lag)
}

void assert_failed(const char* expr, const char* file, int line)
{
   std::printf("ENGINE_ASSERT failed: %s\n  at %s:%d\n", expr, file, line);
   char where[kAssertChars];
   // Global snprintf: KallistiOS' newlib C++ library does not declare std::snprintf.
   snprintf(where, sizeof where, "%s:%d", file, line);
   pvr_shutdown();
   vid_set_mode(DM_640x480, PM_RGB565);
   vid_clear(0, 0, 0);
   bfont_draw_str(vram_s + kTitleTop * kFbWidth + kTextLeft, kFbWidth, true, "ENGINE_ASSERT failed");
   bfont_draw_str(vram_s + kExprTop * kFbWidth + kTextLeft, kFbWidth, true, expr);
   bfont_draw_str(vram_s + kWhereTop * kFbWidth + kTextLeft, kFbWidth, true, where);
   for (;;)
   {
      vid_waitvbl();
   }
}

} // namespace capsule::hal::system

namespace capsule::hal::input
{

KeyBits held()
{
   return g_held;
}

} // namespace capsule::hal::input
