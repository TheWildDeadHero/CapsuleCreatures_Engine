/// @file
/// GBA HAL (P-8): direct hardware access through libgba. Linked only into the
/// GBA target. Display registers and OAM are shadowed here. When a frame is
/// finished, system::update() marks it ready and waits; the VBlank interrupt
/// then commits the registers and lands the VRAM upload queue (A-2, A-5) at
/// the very start of VBlank, so the next frame's logic starts with every
/// upload in place. A frame that overran is not marked ready in time: that
/// VBlank changes nothing, so a frame never shows half-applied state.
#include "capsule/core/assert.h"
#include "capsule/core/memory.h"
#include "capsule/core/random.h"
#include "capsule/core/types.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/hal/vram_queue.h"
#include "capsule/text/font.h"

#include "../common/audio_hook.h"

#include <gba_input.h>
#include <gba_interrupt.h>
#include <gba_systemcalls.h>
#include <gba_video.h>

namespace
{

using namespace capsule::hal;
using namespace capsule::hal::video;   // the GBA model's layout constants

constexpr uint16_t kDispcntModeMask = 0x0007;      ///< DISPCNT display mode bits.
constexpr uint16_t kDispcntForcedBlank = 0x0080;   ///< DISPCNT forced-blank bit.
constexpr uint16_t kDispcntBg0 = 0x0100;           ///< DISPCNT BG0 enable; BGn is this << n.
constexpr uint16_t kDispcntObj1d = 0x0040;         ///< DISPCNT 1D OBJ tile mapping (always on).
constexpr uint16_t kDispcntObj = 0x1000;           ///< DISPCNT sprite layer enable.
constexpr uint16_t kDispcntWin0 = 0x2000;          ///< DISPCNT window 0 enable.
constexpr uint16_t kDispcntWin1 = 0x4000;          ///< DISPCNT window 1 enable.
constexpr uint16_t kDispcntObjWin = 0x8000;        ///< DISPCNT OBJ window enable.
constexpr uint16_t kKeyMask = 0x03FF;              ///< KEYINPUT's 10 button bits.
constexpr uint32_t kIoBase = 0x04000000;           ///< I/O registers.

/// @name I/O register offsets (from kIoBase)
///@{
constexpr uint32_t kRegVcount = 0x06;      ///< VCOUNT: current scanline.
constexpr uint32_t kRegBg2Affine = 0x20;   ///< BG2PA: first of BG2's affine registers.
constexpr uint32_t kBgAffineStride = 0x10; ///< From BG2's affine registers to BG3's.
constexpr uint32_t kAffinePa = 0;          ///< BGnPA, from the layer's first affine register.
constexpr uint32_t kAffinePb = 2;          ///< BGnPB.
constexpr uint32_t kAffinePc = 4;          ///< BGnPC.
constexpr uint32_t kAffinePd = 6;          ///< BGnPD.
constexpr uint32_t kAffineX = 8;           ///< BGnX (32-bit).
constexpr uint32_t kAffineY = 12;          ///< BGnY (32-bit).
constexpr uint32_t kRegWin0h = 0x40;       ///< WIN0H.
constexpr uint32_t kRegWin1h = 0x42;       ///< WIN1H.
constexpr uint32_t kRegWin0v = 0x44;       ///< WIN0V.
constexpr uint32_t kRegWin1v = 0x46;       ///< WIN1V.
constexpr uint32_t kRegWinin = 0x48;       ///< WININ.
constexpr uint32_t kRegWinout = 0x4A;      ///< WINOUT.
constexpr uint32_t kRegMosaic = 0x4C;      ///< MOSAIC.
constexpr uint32_t kRegBldcnt = 0x50;      ///< BLDCNT.
constexpr uint32_t kRegBldalpha = 0x52;    ///< BLDALPHA.
constexpr uint32_t kRegBldy = 0x54;        ///< BLDY.
constexpr uint32_t kRegDma0Sad = 0xB0;     ///< N2: DMA0SAD: source address (the window lines' HBlank DMA).
constexpr uint32_t kRegDma0Dad = 0xB4;     ///< N2: DMA0DAD: destination address.
constexpr uint32_t kRegDma0Cnt = 0xB8;     ///< N2: DMA0CNT: unit count (low half) and control (high half).
constexpr uint32_t kRegDma3Sad = 0xD4;     ///< DMA3SAD: source address.
constexpr uint32_t kRegDma3Dad = 0xD8;     ///< DMA3DAD: destination address.
constexpr uint32_t kRegDma3Cnt = 0xDC;     ///< DMA3CNT: unit count (low half) and control (high half).
constexpr uint32_t kRegTm1Count = 0x104;   ///< TM1CNT_L: timer 1 counter.
constexpr uint32_t kRegTm1Control = 0x106; ///< TM1CNT_H: timer 1 control.
constexpr uint32_t kRegKeycnt = 0x132;     ///< KEYCNT: keypad interrupt control.
constexpr uint32_t kRegWaitcnt = 0x204;    ///< WAITCNT: cartridge wait states.
///@}

/// @name Register fields
///@{
constexpr uint16_t kTimerEnable = 0x0080;       ///< TMxCNT_H bit 7: running (prescaler 0: every cycle).
constexpr uint16_t kKeycntIrq = 0x4000;         ///< KEYCNT bit 14: interrupt (bit 15 clear: on any key).
constexpr uint16_t kBgcntMosaic = 0x40;         ///< BGnCNT bit 6: mosaic.
constexpr int kBgcntCharBlockShift = 2;         ///< BGnCNT bits 2-3: character block.
constexpr int kBgcnt8bpp = 0x80;                ///< BGnCNT bit 7: 8bpp tiles.
constexpr int kBgcntScreenBlockShift = 8;       ///< BGnCNT bits 8-12: first screen block.
constexpr int kBgcntScreenBlockMask = 31;       ///< Mask of the screen block field.
constexpr int kBgcntWrap = 0x2000;              ///< BGnCNT bit 13: affine layer wraps around.
constexpr int kBgcntSizeShift = 14;             ///< BGnCNT bits 14-15: screen size.
constexpr int kBgcntSizeMask = 3;               ///< Mask of the screen size field.
constexpr int kBldcntModeShift = 6;             ///< BLDCNT bits 6-7: BlendMode (bits 0-5: first targets).
constexpr int kBldcntSecondShift = 8;           ///< BLDCNT bits 8-13: second targets.
constexpr int kBldWeightMask = 0x1F;            ///< BLDALPHA / BLDY: 5-bit weight fields.
constexpr int kBldalphaEvbShift = 8;            ///< BLDALPHA bits 8-12: second-target weight.
constexpr int kMosaicFieldMax = 15;             ///< MOSAIC: each field is block size - 1, 0-15.
constexpr int kMosaicBgVShift = 4;              ///< MOSAIC bits 4-7: BG vertical size (bits 0-3: horizontal).
constexpr int kMosaicObjHShift = 8;             ///< MOSAIC bits 8-11: OBJ horizontal size.
constexpr int kMosaicObjVShift = 12;            ///< MOSAIC bits 12-15: OBJ vertical size.
constexpr int kWinStartShift = 8;               ///< WINnH / WINnV bits 8-15: left / top (bits 0-7: right / bottom).
constexpr uint32_t kDma3MaxUnits = 0x10000u;    ///< Most units one DMA3 transfer moves (count 0 means this).
constexpr uint32_t kDma3CountMask = 0xFFFFu;    ///< DMA3CNT bits 0-15: unit count.
///@}

/// @name Memory map
///@{
constexpr uint32_t kBgVram = 0x06000000u;       ///< BG VRAM.
constexpr uint32_t kBiosIf = 0x03007FF8;        ///< BIOS interrupt flags, at the top of IWRAM.
constexpr int kAddressPageShift = 24;           ///< Address bits 24-31: memory area.
constexpr uint32_t kPageEwram = 0x02;           ///< Memory area of EWRAM.
constexpr uint32_t kPageIwram = 0x03;           ///< Memory area of IWRAM.
///@}

/// @name Map geometry
///@{
constexpr uint32_t kWideBlocks = 2;        ///< Screen blocks across a wide regular map.
constexpr uint32_t kTallTiles = 64;        ///< Rows of a tall regular map (two screen blocks).
constexpr int kAffineBgMinCells = 16;      ///< Cells per side of the smallest affine map; each size doubles it.
constexpr int kLowByteMask = 0x00FF;       ///< Low byte of a halfword.
constexpr int kHighByteMask = 0xFF00;      ///< High byte of a halfword.
///@}

/// @name Assert screen (assert_failed())
///@{
constexpr video::Color kAssertBackdrop = video::rgb5(12, 0, 0);   ///< Dark red background.
constexpr video::Color kAssertGlyph = video::rgb5(31, 31, 31);    ///< White text.
constexpr video::Color kAssertShadow = video::rgb5(4, 0, 0);      ///< Near-black text shadow.
constexpr int kAssertColumns = video::kScreenWidth / video::kTileSize;   ///< Characters per line, 30.
constexpr int kAssertRows = video::kScreenHeight / video::kTileSize;     ///< Lines on screen, 20.
constexpr char kFirstPrintable = 0x20;    ///< First printable ASCII character (space).
constexpr char kLastPrintable = 0x7E;     ///< Last printable ASCII character (tilde).
constexpr int kLineDigits = 12;           ///< Room for the decimal digits of a line number.
constexpr unsigned kDecimalBase = 10;     ///< Line numbers are shown in decimal.
///@}

/// @name Other
///@{
constexpr int kHalfwordBits = 16;                    ///< Bits in a halfword: the high half of a word.
constexpr int kHalfwordsPerWord = 2;                 ///< Halfwords in a word.
constexpr int kTimerEntropyShift = 8;                ///< Timer 1 moves up a byte in entropy().
constexpr uint32_t kFrameHash = 0x9E3779B1u;   ///< 2^32 / golden ratio: spreads the frame count over every bit.
constexpr int kOamWords = video::kNumObjs * video::kOamEntryHalfwords / kHalfwordsPerWord;   ///< OAM, in words.
///@}

#ifndef CAPSULE_GBA_WAITCNT
/// WAITCNT at start-up (build setting): prefetch on, ROM wait states 3/1
/// (the reference games' default); flashcarts that need slower ROM access override it.
#define CAPSULE_GBA_WAITCNT 0x4014
#endif
constexpr uint32_t kObjVram = 0x06010000;   ///< OBJ tiles.
constexpr uint32_t kObjPalette = 0x05000200;   ///< OBJ palette.
constexpr uint32_t kOam = 0x07000000;   ///< OAM.
constexpr uint32_t kBgPalette = 0x05000000;   ///< BG palette.
constexpr uint32_t kDma3Enable = 0x80000000u;   ///< DMA3CNT: enable (immediate start).
constexpr uint32_t kDma32 = 0x04000000u;   ///< DMA3CNT: 32-bit units.
constexpr uint32_t kDmaSrcFixed = 0x01000000u;   ///< DMA3CNT: source address fixed (fills).
constexpr uint32_t kDmaStartHblank = 0x20000000u;   ///< N2: DMAnCNT: start at each HBlank.
constexpr uint32_t kDmaRepeat = 0x02000000u;   ///< N2: DMAnCNT: repeat at each start.
constexpr uint32_t kDmaDstFixed = 0x00400000u;   ///< N2: DMAnCNT: destination address fixed.
/// N2: DMA0CNT for the window lines: enabled, started at each HBlank, repeated, destination fixed, one 16-bit unit.
constexpr uint32_t kDma0WindowLines = kDma3Enable | kDmaStartHblank | kDmaRepeat | kDmaDstFixed | 1u;

/// A 16-bit I/O register.
/// @param offset  offset from kIoBase
/// @return the register
volatile uint16_t& io16(uint32_t offset)
{
   return *reinterpret_cast<volatile uint16_t*>(kIoBase + offset);
}

/// A 32-bit I/O register.
/// @param offset  offset from kIoBase
/// @return the register
volatile uint32_t& io32(uint32_t offset)
{
   return *reinterpret_cast<volatile uint32_t*>(kIoBase + offset);
}

/// Runs one immediate DMA3 transfer and waits for it (the CPU is halted while
/// DMA runs, so the wait only covers the start-up cycles).
/// @param src    source address
/// @param dst    destination address
/// @param units  number of 16- or 32-bit units (1-65536; 0 is not allowed)
/// @param flags  kDma32 and/or kDmaSrcFixed
void dma3(const volatile void* src, volatile void* dst, uint32_t units, uint32_t flags)
{
   ENGINE_ASSERT(units > 0 && units <= kDma3MaxUnits);
   io32(kRegDma3Sad) = reinterpret_cast<uint32_t>(src);
   io32(kRegDma3Dad) = reinterpret_cast<uint32_t>(dst);
   // Count and control in one write; 65536 units is a count of 0.
   io32(kRegDma3Cnt) = kDma3Enable | flags | (units & kDma3CountMask);
   while (io32(kRegDma3Cnt) & kDma3Enable)
   {
   }
}

/// Base address of a video memory region.
/// @param region  the region
/// @return its address
uint32_t region_address(video::Region region)
{
   switch (region)
   {
   case video::Region::BgVram:
      return kBgVram;
   case video::Region::ObjVram:
      return kObjVram;
   case video::Region::BgPalette:
      return kBgPalette;
   case video::Region::ObjPalette:
      return kObjPalette;
   case video::Region::NUM_REGIONS:
      break;
   }
   return 0;
}

/// Transform of one affine layer (BG2 or BG3).
struct Affine
{
   int16_t pa;   ///< BGnPA.
   int16_t pb;   ///< BGnPB.
   int16_t pc;   ///< BGnPC.
   int16_t pd;   ///< BGnPD.
   int32_t x;   ///< BGnX.
   int32_t y;   ///< BGnY.
};

/// Display registers as last written; committed at VBlank.
struct Shadow
{
   uint16_t dispcnt;   ///< DISPCNT.
   uint16_t bgcnt[video::kNumBgs];   ///< BGnCNT.
   int16_t hofs[video::kNumBgs];   ///< BGnHOFS.
   int16_t vofs[video::kNumBgs];   ///< BGnVOFS.
   Affine affine[kNumAffineBgs];   ///< BG2 / BG3 transform.
   uint16_t bldcnt;   ///< BLDCNT.
   uint16_t bldalpha;   ///< BLDALPHA.
   uint16_t bldy;   ///< BLDY.
   uint16_t mosaic;   ///< MOSAIC (0: off).
   uint16_t winh[kNumWindows];   ///< WIN0H / WIN1H.
   uint16_t winv[kNumWindows];   ///< WIN0V / WIN1V.
   uint16_t winin;   ///< WININ.
   uint16_t winout;   ///< WINOUT.
   const uint16_t* winlines;   ///< N2: window 0's edges per scanline (video::set_window_lines()), or nullptr.
};

Shadow g_shadow{};   ///< Register shadow.
/// OAM shadow, GBA layout: attr0-2 plus an interleaved affine parameter per entry.
alignas(kWordBytes) uint16_t g_oam[video::kNumObjs * kOamEntryHalfwords];
input::KeyBits g_held = 0;   ///< Buttons read at the last update().
uint32_t g_vblanks = 0;   ///< Frames finished since init().
/// C11: vertical blanks since init(), counted by the VBlank interrupt (lag frames included).
volatile uint32_t g_vblank_irqs = 0;
uint32_t g_blanks_advanced = 0;   ///< C11: the blanks update() made the per-frame RNG advance for.
/// Set by update() when a frame is finished, cleared by the VBlank interrupt
/// once it has committed the frame (the main loop does not touch the shadows
/// or the queue in between: it is waiting).
volatile uint8_t g_frame_ready = 0;
volatile uint32_t g_press_time = 0;   ///< Timer 1 and VCOUNT at the first key press (keypad IRQ).

/// The interrupt master enable (REG_IME) set for a scope: the constructor
/// sets it, the destructor puts back the value it found, on every path.
struct ImeScope
{
   const uint16_t before;   ///< REG_IME when the scope began.

   /// Sets the master enable.
   /// @param on  interrupts allowed inside the scope
   explicit ImeScope(bool on) : before(REG_IME)
   {
      REG_IME = on ? 1 : 0;
   }
   ImeScope(const ImeScope&) = delete;
   ImeScope& operator=(const ImeScope&) = delete;
   /// Restores the master enable.
   ~ImeScope()
   {
      REG_IME = before;
   }
};

/// Keypad interrupt: records the cycle-exact moment of the first key press
/// (A-4), then turns itself off (the keypad IRQ repeats while a key is held).
void on_keypad()
{
   g_press_time = (uint32_t{io16(kRegTm1Count)} << kHalfwordBits) | io16(kRegVcount);
   io16(kRegKeycnt) = 0;   // KEYCNT: no more keypad interrupts
   irqDisable(IRQ_KEYPAD);
}

/// BG VRAM as halfwords (VRAM takes 16- and 32-bit writes only).
/// @return VRAM base
volatile uint16_t* vram16()
{
   return reinterpret_cast<volatile uint16_t*>(VRAM);
}

/// The BG palette RAM.
/// @return palette base
volatile uint16_t* bg_palette()
{
   return reinterpret_cast<volatile uint16_t*>(kBgPalette);
}

/// Halfword index of a map cell, honoring multi-block screen sizes.
/// @param bg  layer
/// @param x   column, tiles
/// @param y   row, tiles
/// @return index into vram16()
uint32_t map_cell_index(video::BgIndex bg, int x, int y)
{
   const uint16_t cnt = g_shadow.bgcnt[bg];
   const uint32_t screen_block = (cnt >> kBgcntScreenBlockShift) & kBgcntScreenBlockMask;
   const uint32_t size = (cnt >> kBgcntSizeShift) & kBgcntSizeMask;
   const uint32_t blocks_wide = (size & 1) ? kWideBlocks : 1;
   const uint32_t width_tiles = kScreenBlockTiles * blocks_wide;
   const uint32_t height_tiles = (size & kScreenSizeTall) ? kTallTiles : kScreenBlockTiles;
   ENGINE_ASSERT(x >= 0 && y >= 0 && static_cast<uint32_t>(x) < width_tiles && static_cast<uint32_t>(y) < height_tiles);

   // Screen blocks of a large map run left to right, then top to bottom.
   const uint32_t block = screen_block + (static_cast<uint32_t>(x) >> kScreenBlockShift) +
                          (static_cast<uint32_t>(y) >> kScreenBlockShift) * blocks_wide;
   return block * kScreenBlockCells + (static_cast<uint32_t>(y) & kScreenBlockTileMask) * kScreenBlockTiles +
          (static_cast<uint32_t>(x) & kScreenBlockTileMask);
}

/// N2: window 0's per-scanline edges of the frame last committed (video::set_window_lines()), or nullptr: a lag
/// frame's VBlank arms their transfer again.
const uint16_t* g_line_winlines = nullptr;

/// N2: (re)starts DMA channel 0 for the committed window lines: line 0's edges written now, then one entry after
/// each line's HBlank (the hardware runs no HBlank DMA during VBlank). Stops the last frame's transfer first, so
/// the repeating transfer never runs past the table's end, a lag frame's included (its source address keeps
/// counting up otherwise).
CAPSULE_IWRAM_CODE void arm_line_dma()
{
   io32(kRegDma0Cnt) = 0;
   const uint16_t* lines = g_line_winlines;
   if (lines == nullptr)
   {
      return;
   }
   io16(kRegWin0h) = lines[0];
   io32(kRegDma0Sad) = reinterpret_cast<uint32_t>(lines + 1);
   io32(kRegDma0Dad) = kIoBase + kRegWin0h;
   io32(kRegDma0Cnt) = kDma0WindowLines;
}

/// Copies the register and OAM shadows to the hardware (called at the start
/// of VBlank). Runs from IWRAM: it is the first thing every VBlank does.
CAPSULE_IWRAM_CODE void commit_registers()
{
   REG_DISPCNT = static_cast<uint16_t>(g_shadow.dispcnt | kDispcntObj1d);
   const uint16_t mosaic_bit = g_shadow.mosaic != 0 ? kBgcntMosaic : 0;   // BGCNT bit 6 on every layer
   for (int bg = 0; bg < video::kNumBgs; ++bg)
   {
      BGCTRL[bg] = static_cast<uint16_t>(g_shadow.bgcnt[bg] | mosaic_bit);
      BG_OFFSET[bg].x = static_cast<uint16_t>(g_shadow.hofs[bg]);
      BG_OFFSET[bg].y = static_cast<uint16_t>(g_shadow.vofs[bg]);
   }
   for (int i = 0; i < kNumAffineBgs; ++i)
   {
      const Affine& a = g_shadow.affine[i];
      const uint32_t base = kRegBg2Affine + kBgAffineStride * static_cast<uint32_t>(i);   // BG2PA / BG3PA
      io16(base + kAffinePa) = static_cast<uint16_t>(a.pa);
      io16(base + kAffinePb) = static_cast<uint16_t>(a.pb);
      io16(base + kAffinePc) = static_cast<uint16_t>(a.pc);
      io16(base + kAffinePd) = static_cast<uint16_t>(a.pd);
      io32(base + kAffineX) = static_cast<uint32_t>(a.x);
      io32(base + kAffineY) = static_cast<uint32_t>(a.y);
   }

   // Windows, color effects and mosaic.
   io16(kRegWin0h) = g_shadow.winh[0];
   io16(kRegWin1h) = g_shadow.winh[1];
   io16(kRegWin0v) = g_shadow.winv[0];
   io16(kRegWin1v) = g_shadow.winv[1];
   io16(kRegWinin) = g_shadow.winin;
   io16(kRegWinout) = g_shadow.winout;
   // N2: per-scanline window 0 edges: line 0's now, then an HBlank DMA writes line y + 1's after line y
   g_line_winlines = g_shadow.winlines;
   arm_line_dma();
   io16(kRegBldcnt) = g_shadow.bldcnt;
   io16(kRegBldalpha) = g_shadow.bldalpha;
   io16(kRegBldy) = g_shadow.bldy;
   io16(kRegMosaic) = g_shadow.mosaic;
   if (g_shadow.mosaic != 0)
   {   // OBJ mosaic: attr0 bit 12 on every entry
      for (int i = 0; i < video::kNumObjs; ++i)
      {
         uint16_t& attr0 = g_oam[i * kOamEntryHalfwords + kOamAttr0];
         attr0 = static_cast<uint16_t>(attr0 | kObjMosaic);
      }
   }
   dma3(g_oam, reinterpret_cast<volatile void*>(kOam), kOamWords, kDma32);
}

/// BIOS interrupt flags (IntrWait), acknowledged by libgba's dispatcher.
/// @return the halfword at kBiosIf (0x03007FF8)
volatile uint16_t& bios_if()
{
   return *reinterpret_cast<volatile uint16_t*>(kBiosIf);
}

/// VBlank interrupt: commits a finished frame (registers, OAM, queued VRAM
/// uploads) at the start of VBlank; commits nothing on a lag frame. M4: then
/// the audio step (audio_hook.h); C14a (D-34, the audio clock): only on the
/// blank that finishes a game frame, so a lag frame of this build steps no
/// sound (the backend repeats the segment the DMA played instead).
void on_vblank()
{
   g_vblank_irqs = g_vblank_irqs + 1;   // C11: every blank counts, a lag frame's too (vblanks())
   const bool frame = g_frame_ready != 0;
   if (frame)
   {
      commit_registers();
      // libgba's dispatcher calls handlers with IME off; the flush can take most
      // of VBlank, so other interrupts (keypad, the audio VCOUNT, timers, serial)
      // may nest meanwhile. A second VBlank cannot arrive before this one ends.
      {
         const ImeScope nested(true);
         vram::flush();
      }
      g_frame_ready = 0;
   }
   else
   {
      arm_line_dma();   // N2: a lag frame shows the committed window lines again (never past the table's end)
   }
   // M4 (D-34): the sound driver steps after the frame's commit (nothing when audio was never started: no audio
   // code is linked then). C14a (the audio clock): once per game frame, as on every other target; this build's own
   // lag blanks step nothing, the reference's modelled ones step through rng::spend_blanks()
   if (frame && audio::hook::g_vblank_step != nullptr)
   {
      const ImeScope nested(true);
      audio::hook::g_vblank_step();
   }
}

/// Resets the shadows: registers cleared, identity affine transforms, every
/// sprite hidden.
void reset_shadow()
{
   g_shadow = Shadow{};
   for (Affine& a : g_shadow.affine)
   {
      a.pa = capsule::kQ8One;   // identity transform
      a.pd = capsule::kQ8One;
   }
   for (int i = 0; i < video::kNumObjs; ++i)
   {
      uint16_t* entry = g_oam + i * kOamEntryHalfwords;
      entry[kOamAttr0] = video::kObjHidden.attr0;
      entry[kOamAttr1] = 0;
      entry[kOamAttr2] = 0;
      entry[kOamAffineHalfword] = 0;
   }
}

}   // namespace

namespace capsule::hal::system
{

void init()
{
   irqInit();
   irqSet(IRQ_VBLANK, on_vblank);   // A-2: the frame lands in the VBlank interrupt
   irqEnable(IRQ_VBLANK);
   io16(kRegWaitcnt) = CAPSULE_GBA_WAITCNT;   // A-2: ROM wait states and prefetch (build setting)
   io16(kRegTm1Count) = 0;   // Timer 1: free running at 16.78 MHz, entropy for A-4
   io16(kRegTm1Control) = kTimerEnable;
   irqSet(IRQ_KEYPAD, on_keypad);   // the first press, timed to the cycle (A-4)
   irqEnable(IRQ_KEYPAD);
   io16(kRegKeycnt) = kKeycntIrq | kKeyMask;   // KEYCNT: interrupt on any of the 10 keys
   vram::reset();
   // Forced blank until the first update(), so setup is never visible.
   reset_shadow();
   g_shadow.dispcnt = kDispcntForcedBlank;
   REG_DISPCNT = g_shadow.dispcnt;
   g_blanks_advanced = g_vblank_irqs;   // C11: the boot's blanks (before the first frame) draw nothing
   rng::set_blank_hook(audio::hook::spend);   // C14a (D-34): modelled lag blanks step the sound too
   rng::seed(entropy());
}

uint32_t entropy()
{
   // Read from the main loop, Timer 1 and VCOUNT depend only on the frame
   // number (the code before is the same every boot). The keypad interrupt's
   // reading does not: it is the cycle at which the player's finger landed.
   return g_press_time ^ (uint32_t{io16(kRegTm1Count)} << kTimerEntropyShift) ^ (g_vblanks * kFrameHash);
}

bool update()
{
   g_shadow.dispcnt &= static_cast<uint16_t>(~kDispcntForcedBlank);
   // Hand the frame to the VBlank interrupt. The BIOS flag is cleared with
   // interrupts off, so IntrWait(0, ...) returns after the first VBlank that
   // saw the frame ready, even if that VBlank starts before the wait does.
   // Every write of the frame (shadows, OAM, queued requests) must be done
   // before the interrupt may read them: a compiler barrier on each side.
   asm volatile("" ::: "memory");
   {
      const ImeScope quiet(false);
      bios_if() = static_cast<uint16_t>(bios_if() & ~IRQ_VBLANK);
      g_frame_ready = 1;
   }
   IntrWait(0, IRQ_VBLANK);
   asm volatile("" ::: "memory");
   g_held = static_cast<input::KeyBits>(~REG_KEYINPUT & kKeyMask);
   ++g_vblanks;
   // N1: a game's per-frame draw at the vertical blank (off by default); C11 (R8 finding 6): once for each blank
   // since the last update(), so the blanks of a lag frame or a blocking save write draw too
   const uint32_t blanks = g_vblank_irqs;
   if (rng::advance_clock() == rng::AdvanceClock::Frame)
   {
      rng::frame_advance();   // C13b: one draw per frame, as the desktop and headless HALs (lag frames add none)
      g_blanks_advanced = blanks;
   }
   for (; g_blanks_advanced != blanks; ++g_blanks_advanced)
   {
      rng::frame_advance();
   }
   seed_on_first_press(g_held);
   return true;   // a cartridge never asks to quit
}

uint32_t vblanks()
{
   return g_vblank_irqs;
}

// P-15: halt and show the failed expression, file and line on screen, using
// the engine's default font on BG0.
void assert_failed(const char* expr, const char* file, int line)
{
   REG_IME = 0;
   vram::reset();   // nothing queued may land over the message (or fail again)
   REG_DISPCNT = kDispcntForcedBlank;
   io16(kRegBldcnt) = 0;   // no color effects over the message

   constexpr uint8_t kCharBlock = 0;
   constexpr uint8_t kScreenBlock = 31;
   constexpr uint16_t kColors = 3;   // backdrop, glyph, shadow
   static constexpr video::Color palette[kColors] = {kAssertBackdrop, kAssertGlyph, kAssertShadow};
   video::load_bg_palette(0, palette, kColors);
   text::load_font(text::kDefaultFont, text::kDefaultFontGlyphs, kCharBlock, 0);
   g_shadow = Shadow{};
   g_shadow.bgcnt[0] = static_cast<uint16_t>(kScreenBlock << kBgcntScreenBlockShift);

   // Clear the whole 32x32 screen block to blanks.
   const auto blank = static_cast<video::TileIndex>(' ' - text::kDefaultFont.first_code);
   for (int y = 0; y < kScreenBlockTiles; ++y)
   {
      for (int x = 0; x < kScreenBlockTiles; ++x)
      {
         video::write_map_cell(0, x, y, video::map_cell(blank, 0));
      }
   }

   int x = 0;
   int y = 1;
   auto put = [&](char c)
   {
      if (c == '\n' || x == kAssertColumns)
      {
         x = 0;
         ++y;
      }
      if (c == '\n' || y >= kAssertRows)
      {
         return;
      }
      const char shown = (c >= kFirstPrintable && c <= kLastPrintable) ? c : '?';
      const auto tile = static_cast<video::TileIndex>(shown - text::kDefaultFont.first_code);
      video::write_map_cell(0, x++, y, video::map_cell(tile, 0));
   };
   auto put_str = [&](const char* s)
   {
      while (*s)
      {
         put(*s++);
      }
   };

   // Show only the file name, not its directory.
   const char* name = file;
   for (const char* p = file; *p; ++p)
   {
      if (*p == '/' || *p == '\\')
      {
         name = p + 1;
      }
   }

   put_str("ENGINE_ASSERT failed\n\n");
   put_str(expr);
   put_str("\n\n");
   put_str(name);
   put(':');
   char digits[kLineDigits];
   int n = 0;
   unsigned value = line < 0 ? 0u : static_cast<unsigned>(line);
   do
   {   // least significant digit first; printed in reverse below
      digits[n++] = static_cast<char>('0' + value % kDecimalBase);
      value /= kDecimalBase;
   }
   while (value != 0 && n < kLineDigits - 1);
   while (n > 0)
   {
      put(digits[--n]);
   }

   REG_DISPCNT = static_cast<uint16_t>(MODE_0 | kDispcntBg0);
   BGCTRL[0] = g_shadow.bgcnt[0];
   BG_OFFSET[0].x = 0;
   BG_OFFSET[0].y = 0;
   while (true)
   {
   }
}

}   // namespace capsule::hal::system

namespace capsule::hal::input
{

KeyBits held()
{
   return g_held;
}

}   // namespace capsule::hal::input

namespace capsule::hal::video
{

void set_mode(uint8_t mode)
{
   ENGINE_ASSERT(mode <= kModeAffine);
   g_shadow.dispcnt = static_cast<uint16_t>((g_shadow.dispcnt & ~kDispcntModeMask) | mode);
}

void configure_bg(BgIndex bg, bool on, uint8_t priority, uint8_t char_block, uint8_t screen_block, bool color_8bpp,
                  uint8_t screen_size)
{
   ENGINE_ASSERT(bg < kNumBgs && priority < kNumPriorities && char_block < kNumCharBlocks &&
                 screen_block < kNumScreenBlocks && screen_size < kNumScreenSizes);
   const uint16_t wrap = static_cast<uint16_t>(g_shadow.bgcnt[bg] & kBgcntWrap);   // kept across configure
   g_shadow.bgcnt[bg] =
      static_cast<uint16_t>(priority | (char_block << kBgcntCharBlockShift) | (color_8bpp ? kBgcnt8bpp : 0) |
                            (screen_block << kBgcntScreenBlockShift) | wrap | (screen_size << kBgcntSizeShift));
   const auto enable = static_cast<uint16_t>(kDispcntBg0 << bg);
   g_shadow.dispcnt = static_cast<uint16_t>(on ? (g_shadow.dispcnt | enable) : (g_shadow.dispcnt & ~enable));
}

void set_bg_scroll(BgIndex bg, int16_t x, int16_t y)
{
   ENGINE_ASSERT(bg < kNumBgs);
   g_shadow.hofs[bg] = x;
   g_shadow.vofs[bg] = y;
}

int16_t bg_scroll_x(BgIndex bg)
{
   return g_shadow.hofs[bg];
}
int16_t bg_scroll_y(BgIndex bg)
{
   return g_shadow.vofs[bg];
}

void load_bg_tiles(uint8_t char_block, TileIndex first_tile, const uint32_t* data, uint16_t tile_count)
{
   ENGINE_ASSERT(char_block < kNumCharBlocks);
   const uint32_t offset = char_block * kCharBlockBytes + first_tile * uint32_t{kTileBytes4bpp};
   ENGINE_ASSERT(offset + tile_count * uint32_t{kTileBytes4bpp} <= kBgVramBytes);
   vram::before_immediate(Region::BgVram, offset, tile_count * uint32_t{kTileBytes4bpp});
   // 32-bit writes: an 8-bit write to BG VRAM stores the byte in both halves.
   volatile uint32_t* dst = reinterpret_cast<volatile uint32_t*>(VRAM + offset);
   const uint32_t words = tile_count * uint32_t{kTileBytes4bpp / kWordBytes};
   for (uint32_t i = 0; i < words; ++i)
   {
      dst[i] = data[i];
   }
}

void load_bg_palette(uint8_t first_color, const Color* colors, uint16_t count)
{
   ENGINE_ASSERT(first_color + count <= kPaletteColors);
   vram::before_immediate(Region::BgPalette, first_color * kHalfwordBytes, count * kHalfwordBytes);
   for (uint16_t i = 0; i < count; ++i)
   {
      bg_palette()[first_color + i] = colors[i];
   }
}

void write_map_cell(BgIndex bg, int x, int y, MapCell cell)
{
   const uint32_t index = map_cell_index(bg, x, y);
   vram::before_immediate(Region::BgVram, index * kHalfwordBytes, kHalfwordBytes);
   vram16()[index] = cell;
}

void write_map_row(BgIndex bg, int x, int y, const MapCell* cells, int count)
{
   if (count <= 0)
   {
      return;
   }
   // One queue check for the whole row: from its first cell to its last (a
   // row that crosses into the next screen block covers the gap too, which
   // only makes the check more cautious).
   const uint32_t first = map_cell_index(bg, x, y);
   const uint32_t last = map_cell_index(bg, x + count - 1, y);
   vram::before_immediate(Region::BgVram, first * kHalfwordBytes, (last - first + 1) * kHalfwordBytes);
   uint32_t index = first;
   for (int i = 0; i < count; ++i)
   {
      if (i != 0 && ((x + i) & kScreenBlockTileMask) == 0)
      {   // the next screen block of a wide layer
         index = map_cell_index(bg, x + i, y);
      }
      vram16()[index++] = cells[i];
   }
}

MapCell read_map_cell(BgIndex bg, int x, int y)
{
   return vram16()[map_cell_index(bg, x, y)];
}

void set_bg_affine(BgIndex bg, int16_t pa, int16_t pb, int16_t pc, int16_t pd, int32_t ref_x, int32_t ref_y)
{
   ENGINE_ASSERT(bg == kFirstAffineBg || bg == kLastAffineBg);
   g_shadow.affine[bg - kFirstAffineBg] = Affine{pa, pb, pc, pd, ref_x, ref_y};
}

void set_bg_wrap(BgIndex bg, bool wrap)
{
   ENGINE_ASSERT(bg == kFirstAffineBg || bg == kLastAffineBg);
   g_shadow.bgcnt[bg] =
      static_cast<uint16_t>(wrap ? (g_shadow.bgcnt[bg] | kBgcntWrap) : (g_shadow.bgcnt[bg] & ~kBgcntWrap));
}

void load_bg_tiles_8bpp(uint8_t char_block, TileIndex first_tile, const uint32_t* data, uint16_t tile_count)
{
   ENGINE_ASSERT(char_block < kNumCharBlocks);
   const uint32_t offset = char_block * kCharBlockBytes + first_tile * uint32_t{kTileBytes8bpp};
   ENGINE_ASSERT(offset + tile_count * uint32_t{kTileBytes8bpp} <= kBgVramBytes);
   vram::before_immediate(Region::BgVram, offset, tile_count * uint32_t{kTileBytes8bpp});
   volatile uint32_t* dst = reinterpret_cast<volatile uint32_t*>(VRAM + offset);
   const uint32_t words = tile_count * uint32_t{kTileBytes8bpp / kWordBytes};
   for (uint32_t i = 0; i < words; ++i)
   {
      dst[i] = data[i];
   }
}

void write_affine_cell(BgIndex bg, int x, int y, uint8_t tile)
{
   ENGINE_ASSERT(bg == kFirstAffineBg || bg == kLastAffineBg);
   const uint16_t cnt = g_shadow.bgcnt[bg];
   const int cells = kAffineBgMinCells << ((cnt >> kBgcntSizeShift) & kBgcntSizeMask);
   ENGINE_ASSERT(x >= 0 && y >= 0 && x < cells && y < cells);
   const uint32_t addr = ((cnt >> kBgcntScreenBlockShift) & kBgcntScreenBlockMask) * kScreenBlockBytes +
                         static_cast<uint32_t>(y * cells + x);
   // VRAM takes no byte writes: read-modify-write the halfword.
   vram::before_immediate(Region::BgVram, addr & ~1u, kHalfwordBytes);
   volatile uint16_t& half = vram16()[addr >> 1];
   const uint16_t old = half;
   // An odd address is the high byte of its halfword (little-endian).
   half = (addr & 1) ? static_cast<uint16_t>((old & kLowByteMask) | (tile << capsule::kBitsPerByte))
                     : static_cast<uint16_t>((old & kHighByteMask) | tile);
}

void set_obj_layer(bool on)
{
   g_shadow.dispcnt = static_cast<uint16_t>(on ? (g_shadow.dispcnt | kDispcntObj) : (g_shadow.dispcnt & ~kDispcntObj));
}

void set_obj(uint8_t index, ObjAttr attr)
{
   ENGINE_ASSERT(index < kNumObjs);
   uint16_t* entry = g_oam + index * kOamEntryHalfwords;
   entry[kOamAttr0] = attr.attr0;
   entry[kOamAttr1] = attr.attr1;
   entry[kOamAttr2] = attr.attr2;
}

void set_obj_affine(uint8_t group, int16_t pa, int16_t pb, int16_t pc, int16_t pd)
{
   ENGINE_ASSERT(group < kNumObjAffine);
   const int16_t p[kAffineParams] = {pa, pb, pc, pd};
   for (int i = 0; i < kAffineParams; ++i)
   {   // parameter i of matrix n is in the spare halfword of OAM entry 4n + i
      g_oam[(group * kAffineParams + i) * kOamEntryHalfwords + kOamAffineHalfword] = static_cast<uint16_t>(p[i]);
   }
}

void load_obj_tiles(TileIndex first_tile, const uint32_t* data, uint16_t tile_count)
{
   const uint32_t offset = first_tile * uint32_t{kTileBytes4bpp};
   ENGINE_ASSERT(offset + tile_count * uint32_t{kTileBytes4bpp} <= kObjVramBytes);
   vram::before_immediate(Region::ObjVram, offset, tile_count * uint32_t{kTileBytes4bpp});
   volatile uint32_t* dst = reinterpret_cast<volatile uint32_t*>(kObjVram + offset);
   const uint32_t words = tile_count * uint32_t{kTileBytes4bpp / kWordBytes};
   for (uint32_t i = 0; i < words; ++i)
   {
      dst[i] = data[i];
   }
}

void load_obj_palette(uint8_t first_color, const Color* colors, uint16_t count)
{
   ENGINE_ASSERT(first_color + count <= kPaletteColors);
   vram::before_immediate(Region::ObjPalette, first_color * kHalfwordBytes, count * kHalfwordBytes);
   volatile uint16_t* pal = reinterpret_cast<volatile uint16_t*>(kObjPalette);
   for (uint16_t i = 0; i < count; ++i)
   {
      pal[first_color + i] = colors[i];
   }
}

void set_blend(BlendMode mode, uint8_t first, uint8_t second)
{
   ENGINE_ASSERT(mode < BlendMode::NUM_BLEND_MODES);
   g_shadow.bldcnt = static_cast<uint16_t>((first & kLayerAll) | (static_cast<uint16_t>(mode) << kBldcntModeShift) |
                                           ((second & kLayerAll) << kBldcntSecondShift));
}

void set_blend_alpha(uint8_t eva, uint8_t evb)
{
   g_shadow.bldalpha = static_cast<uint16_t>((eva & kBldWeightMask) | ((evb & kBldWeightMask) << kBldalphaEvbShift));
}

void set_blend_brightness(uint8_t evy)
{
   g_shadow.bldy = static_cast<uint16_t>(evy & kBldWeightMask);
}

void set_mosaic(uint8_t size)
{
   // Each MOSAIC field holds block size - 1; the same size goes in all four.
   const auto s = static_cast<uint16_t>(size > kMosaicMax ? kMosaicFieldMax : (size == 0 ? 0 : size - 1));
   g_shadow.mosaic =
      size <= 1 ? uint16_t{0}
                : static_cast<uint16_t>(s | (s << kMosaicBgVShift) | (s << kMosaicObjHShift) | (s << kMosaicObjVShift));
}

void set_window_rect(uint8_t win, uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
   ENGINE_ASSERT(win < kNumWindows && x0 <= x1 && y0 <= y1 && x1 <= kScreenWidth && y1 <= kScreenHeight);
   g_shadow.winh[win] = static_cast<uint16_t>((x0 << kWinStartShift) | x1);
   g_shadow.winv[win] = static_cast<uint16_t>((y0 << kWinStartShift) | y1);
}

void set_window_masks(uint8_t in0, uint8_t in1, uint8_t outside, uint8_t obj)
{
   // WININ: window 0 low byte, window 1 high byte; WINOUT: outside low, OBJ window high.
   g_shadow.winin = static_cast<uint16_t>((in0 & kLayerAll) | ((in1 & kLayerAll) << capsule::kBitsPerByte));
   g_shadow.winout = static_cast<uint16_t>((outside & kLayerAll) | ((obj & kLayerAll) << capsule::kBitsPerByte));
}

void set_window_lines(const uint16_t* lines)
{
   g_shadow.winlines = lines;
}

void set_windows(bool win0, bool win1, bool obj_win)
{
   uint16_t d = static_cast<uint16_t>(g_shadow.dispcnt & ~(kDispcntWin0 | kDispcntWin1 | kDispcntObjWin));
   if (win0)
   {
      d = static_cast<uint16_t>(d | kDispcntWin0);
   }
   if (win1)
   {
      d = static_cast<uint16_t>(d | kDispcntWin1);
   }
   if (obj_win)
   {
      d = static_cast<uint16_t>(d | kDispcntObjWin);
   }
   g_shadow.dispcnt = d;
}

void transfer32(Region region, uint32_t offset, const uint32_t* src, uint32_t words)
{
   ENGINE_ASSERT(region < Region::NUM_REGIONS && offset % kWordBytes == 0 &&
                 offset + words * kWordBytes <= region_bytes(region));
   if (words == 0)
   {
      return;
   }
   dma3(src, reinterpret_cast<volatile void*>(region_address(region) + offset), words, kDma32);
}

void transfer16(Region region, uint32_t offset, const uint16_t* src, uint32_t halfwords)
{
   ENGINE_ASSERT(region < Region::NUM_REGIONS && offset % kHalfwordBytes == 0 &&
                 offset + halfwords * kHalfwordBytes <= region_bytes(region));
   if (halfwords == 0)
   {
      return;
   }
   dma3(src, reinterpret_cast<volatile void*>(region_address(region) + offset), halfwords, 0);
}

void fill32(Region region, uint32_t offset, uint32_t value, uint32_t words)
{
   ENGINE_ASSERT(region < Region::NUM_REGIONS && offset % kWordBytes == 0 &&
                 offset + words * kWordBytes <= region_bytes(region));
   if (words == 0)
   {
      return;
   }
   static volatile uint32_t s_fill;   // DMA reads the fixed source from IWRAM
   s_fill = value;
   dma3(&s_fill, reinterpret_cast<volatile void*>(region_address(region) + offset), words, kDma32 | kDmaSrcFixed);
}

Source classify(const void* p, Source)
{
   const uint32_t page = reinterpret_cast<uint32_t>(p) >> kAddressPageShift;
   if (page == kPageIwram)
   {
      return Source::Iwram;
   }
   if (page == kPageEwram)
   {
      return Source::Ewram;
   }
   return Source::Rom;   // 0x08-0x0D: cartridge ROM (wait-state mirrors)
}

}   // namespace capsule::hal::video
