/// @file
/// Pipelines (D-20 D): the roll every pipeline and the default encounter level draw with (battle::pipeline_roll()).
/// It needs only the random streams, so programs that link the empty battle data (src/battle/battle_empty.cpp)
/// without the battle modules link this file instead of src/battle/pipeline.cpp: linkers that resolve every
/// symbol before dropping unused sections (lld, Apple's ld64) need the definition even when nothing calls it.
#include "capsule/battle/pipeline.h"

namespace capsule::battle
{

int32_t pipeline_roll(PipelineCtx& c, int32_t span)
{
   if (span <= 0)
   {
      return 0;
   }
   if (c.fixed_roll >= 0)
   {
      return c.fixed_roll < span ? c.fixed_roll : span - 1;
   }
   if (c.draw >= 0)
   {
      // the draw scaled to [0, span) exactly as rng::below() scales one
      return static_cast<int32_t>((static_cast<uint32_t>(c.draw) * static_cast<uint32_t>(span)) >> rng::kDrawBits);
   }
   if (c.rng != nullptr)
   {
      return static_cast<int32_t>(rng::below(*c.rng, static_cast<uint32_t>(span)));
   }
   return 0;
}

} // namespace capsule::battle
