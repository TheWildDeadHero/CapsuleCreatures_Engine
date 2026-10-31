/// @file
/// The audio step pointers (audio_hook.h): empty until an audio backend's init() installs its steps.
#include "audio_hook.h"

namespace capsule::hal::audio::hook
{

Step g_vblank_step = nullptr;
SpendStep g_spend_step = nullptr;

void spend(uint32_t blanks)
{
   const SpendStep step = g_spend_step;
   if (step != nullptr)
   {
      step(blanks);
   }
}

} // namespace capsule::hal::audio::hook
