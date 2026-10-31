/// @file
/// The default question of a transformation (capsule/script/growth_port.h ask_open() / ask_poll(), G4): the text
/// box's choice menu with the new species' name (go on) and, when it may be stopped, the species' own (stop; Back
/// too). A game shows its own scene by defining these two functions.
#include "capsule/battle/data.h"
#include "capsule/script/growth_port.h"
#include "capsule/script/port.h"

namespace capsule::script::growth
{

namespace
{

/// Options of the question: go on, stop.
constexpr uint8_t kAskOptions = 2;
/// The option that lets it happen.
constexpr uint8_t kGoOption = 0;

/// The question's state.
struct Ask
{
   const text::Code* options[kAskOptions];   ///< The species' names.
   bool cancellable;                         ///< Whether stop is offered.
};

/// The state (zero at start-up).
Ask g_ask;

} // namespace

void ask_open(uint8_t slot, uint16_t from, uint16_t into, bool cancellable)
{
   (void)slot;
   g_ask.options[kGoOption] = into < battle::kData.species_count ? battle::kData.species[into].name : nullptr;
   g_ask.options[kGoOption + 1] = from < battle::kData.species_count ? battle::kData.species[from].name : nullptr;
   g_ask.cancellable = cancellable;
   const uint8_t count = cancellable ? kAskOptions : 1;
   port::choice_open(g_ask.options, count, cancellable ? static_cast<uint8_t>(kGoOption + 1) : kNoCancel);
}

int8_t ask_poll()
{
   const int16_t a = port::choice_poll();
   if (a < 0)
   {
      return kAskWaiting;
   }
   return a == kGoOption || !g_ask.cancellable ? kAskGo : kAskStop;
}

} // namespace capsule::script::growth
