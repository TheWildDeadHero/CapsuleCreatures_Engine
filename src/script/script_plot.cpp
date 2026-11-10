/// @file
/// M6a K2: the script Plot statements' port (capsule/script/port.h world::plot_op()) over the engine's plots
/// (capsule/plots/plots.h). Weak: a game may answer from its own plots. A program without the engine library links
/// src/script/script_plot_none.cpp instead. M6a K3: At asks the shown map (capsule/map/map.h current()).
#include "capsule/core/memory.h"
#include "capsule/map/map.h"
#include "capsule/plots/plots.h"
#include "capsule/script/port.h"

namespace capsule::script::world
{

namespace
{

constexpr int32_t kMaxId = 0xFFFF;   ///< Plot ids and crops are 16 bits: larger ones are no plot or crop.

static_assert(kNoPlot == plots::kNoPlot, "the script's and the plots' no-plot answers agree");

} // namespace

CAPSULE_WEAK int32_t plot_op(PlotOp kind, int32_t id, int32_t crop)
{
   if (kind == PlotOp::At)
   {
      // K3: id is the object's local ID on the shown map
      const map::Map* m = map::current();
      const bool local = id >= 0 && id < map::kMaxObjectTemplates;
      return m != nullptr && local ? plots::plot_at(map::index_of(*m), static_cast<uint8_t>(id)) : kNoPlot;
   }
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
   case PlotOp::Item:
      return plots::item(plot);
   default:
      return 0;
   }
}

} // namespace capsule::script::world
