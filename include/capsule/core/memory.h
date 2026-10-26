/// @file
/// Placement of large static buffers and of hot code (A-3).
///
/// On the GBA, zero-initialised statics go to the 32 KB IWRAM by default.
/// Buffers that are large and not speed-critical (map ring mirrors, staging
/// buffers, battle state, sequence players) belong in the 256 KB EWRAM
/// instead; CAPSULE_EWRAM_BSS puts them there (devkitARM's ".sbss" section,
/// cleared at start-up like .bss). On other targets it does nothing.
///
/// Data in EWRAM is slower to copy (16-bit bus, 2 wait states): pass
/// hal::video::Source::Ewram for it to the VRAM upload queue. Buffers the DMA
/// or the VBlank interrupt read every frame (OAM shadow, visible palettes)
/// stay in IWRAM.
///
/// CAPSULE_IWRAM_CODE runs a function from IWRAM (32-bit bus, no wait
/// states) as ARM code instead of Thumb code from the cartridge (16-bit bus,
/// ROM wait states). IWRAM is small and shared with the stack and every
/// IWRAM static, so it is only for short loops that run every frame (the
/// VRAM queue's landing loop, the VBlank register commit): check the
/// function's size in the map file before adding one. The function is never
/// inlined (an inlined copy would run from ROM) and is called with a long
/// call (ROM and IWRAM are too far apart for a direct branch).
///
/// CAPSULE_ARM_CODE (C14a) compiles a function as ARM code that stays in the
/// cartridge: for a few instructions the Thumb set lacks (a 32 x 32 -> 64-bit
/// umull), where IWRAM space is not worth it. Never inlined (it must stay
/// ARM); the call is an interworking branch.
#pragma once

#if defined(CAPSULE_PLATFORM_GBA)
/// Places a zero-initialised static in EWRAM (GBA); nothing elsewhere.
#define CAPSULE_EWRAM_BSS __attribute__((section(".sbss")))
/// Runs a function from IWRAM as ARM code (GBA); nothing elsewhere.
#define CAPSULE_IWRAM_CODE __attribute__((section(".iwram"), long_call, noinline, target("arm")))
/// Compiles a function as ARM code in ROM (GBA); nothing elsewhere.
#define CAPSULE_ARM_CODE __attribute__((noinline, target("arm")))
#else
/// Places a zero-initialised static in EWRAM (GBA); nothing elsewhere.
#define CAPSULE_EWRAM_BSS
/// Runs a function from IWRAM as ARM code (GBA); nothing elsewhere.
#define CAPSULE_IWRAM_CODE
/// Compiles a function as ARM code in ROM (GBA); nothing elsewhere.
#define CAPSULE_ARM_CODE
#endif

/// A default definition a program's own one replaces at link time (C9: the empty script ports capsule_game() links
/// for a game without combination methods or eggs: a game that defines its own port keeps it, no duplicate symbol).
#define CAPSULE_WEAK __attribute__((weak))
