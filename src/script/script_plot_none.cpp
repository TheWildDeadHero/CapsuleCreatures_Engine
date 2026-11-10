/// @file
/// M6a K2: the script Plot statements' port of a program without the engine library's plots (the tests that build
/// the script VM themselves): no plot changes, every answer 0. Weak, like the engine library's
/// src/script/script_plot.cpp.
#include "capsule/core/memory.h"
#include "capsule/script/port.h"

namespace capsule::script::world
{

CAPSULE_WEAK int32_t plot_op(PlotOp kind, int32_t id, int32_t crop)
{
   (void)kind;
   (void)id;
   (void)crop;
   return 0;
}

} // namespace capsule::script::world
