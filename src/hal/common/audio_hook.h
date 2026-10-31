/// @file
/// The audio step a system HAL runs at each vertical blank (D-34, M4 B1b), installed by the audio backend's
/// hal::audio::init(). Through this pointer the system HALs (hal_gba.cpp, hal_sdl2.cpp, hal_headless.cpp,
/// hal_dc.cpp) never name the audio core, so a program that never starts audio links none of it, and its frames
/// are what they were before audio existed.
///
/// C14a (D-34, the audio clock, capsule/hal/audio.h): the step runs once per game frame. GBA: on_vblank() runs it on
/// the blank that finishes a frame (after its video commit, with nested interrupts allowed), not on this build's
/// own lag blanks; the step is core::vsync() (not the first) then core::vblank(). Desktop, headless and Dreamcast:
/// system::update() runs it once (they do not lag); the step is core::vsync(), core::vblank(),
/// hal::audio::frame_done(). The reference's modelled lag blanks step through rng::spend_blanks() (the blank hook
/// the system HAL's init() connects to spend()), never through g_vblank_step.
#pragma once

#include <cstdint>

namespace capsule::hal::audio::hook
{

/// One vertical blank's audio step.
using Step = void (*)();

/// The installed step (nullptr: audio was never started). Defined in src/hal/common/audio_hook.cpp.
extern Step g_vblank_step;

/// Runs the installed step, if any.
inline void vblank()
{
   const Step step = g_vblank_step;
   if (step != nullptr)
   {
      step();
   }
}

/// C14a: steps for modelled lag blanks (the backend's: one step each, now; nullptr: audio was never started).
using SpendStep = void (*)(uint32_t blanks);

/// The installed modelled-blank step (nullptr until a backend's init()). Defined in src/hal/common/audio_hook.cpp.
extern SpendStep g_spend_step;

/// C14a (the audio clock): runs @p blanks steps for modelled lag blanks, if audio started. Every system HAL's init()
/// installs this as rng::set_blank_hook(), so rng::spend_blanks() steps the sound too; a tool without the system HAL
/// (apps/audio_render) calls it itself.
/// @param blanks  the modelled blanks
void spend(uint32_t blanks);

} // namespace capsule::hal::audio::hook
