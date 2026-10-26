/// @file
/// Engine default game hooks for the sequence player: no conditions hold,
/// every list is empty, and the title press seeds the random streams with
/// the frame count and the platform's entropy (A-4). A game builds its own
/// file defining these instead of this one (sequence_port.h, namespace game).
#include "capsule/seq/sequence_port.h"

#include "capsule/core/random.h"
#include "capsule/hal/system.h"

namespace capsule::seq::game
{

bool cond(uint8_t)
{
   return false;
}
uint16_t list_count(uint8_t)
{
   return 0;
}
AssetKey list_asset(uint8_t, uint16_t)
{
   return AssetKey{0};
}
void rng_mix_entropy(uint32_t frame_counter)
{
   rng::seed(frame_counter ^ hal::system::entropy());
}

} // namespace capsule::seq::game
