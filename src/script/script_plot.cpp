/// @file
/// M6a K2: the script Plot statements' port (capsule/script/port.h world::plot_op()) over the engine's plots
/// (capsule/plots/plots.h). Weak: a game may answer from its own plots. A program without the engine library links
/// src/script/script_plot_none.cpp instead.
#include "capsule/core/memory.h"
#include "capsule/plots/plots.h"
#include "capsule/script/port.h"

namespace capsule::script::world
{

namespace
{

constexpr int32_t kMaxId = 0xFFFF;   ///< Plot ids and crops are 16 bits: larger ones are no plot or crop.

} // namespace

CAPSULE_WEAK int32_t plot_op(PlotOp kind, int32_t id, int32_t crop)
{
   if (id < 0 || id > kMaxId)
   {
      return kind == PlotOp::Stage ? plots::kStageEmpty : 0;
   }
   const auto plot = static_cast<uint16_t>(id);
   switch (kind)
   {
   case PlotOp::Plant:
      plots::plant(plot, crop < 0 || crop > kMaxId ? plots::kNoCrop : static_cast<uint16_t>(crop));
      return 0;
   case PlotOp::Water:
      plots::water(plot);
      return 0;
   case PlotOp::Clear:
      plots::clear(plot);
      return 0;
   case PlotOp::Pick:
      return plots::pick(plot);
   case PlotOp::Stage:
      return plots::state(plot);
   case PlotOp::Crop:
      return plots::crop(plot);
   case PlotOp::Yield:
      return plots::yield(plot);
   default:
      return 0;
   }
}

} // namespace capsule::script::world
