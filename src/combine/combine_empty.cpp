/// @file
/// The engine's default combination tables (capsule/combine/data.h, B1): no methods (B2: no eggs, no
/// nursery). A build with battle data links
/// its generated tables and function (tools/battlec/combine.py, in the generated battle.cpp) instead, which define
/// every symbol of this file and so keep it out of the link.
#include "capsule/combine/data.h"

namespace capsule::combine
{

const CombineData kCombine = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0,
                              // B2: no eggs, no nursery
                              {nullptr, 0, 0, 0, monster::kNoRole, monster::kNoRole, kNoField, kNoField, kNoStepEffect, 0},
                              {0, 0, 0, kNoStepEffect, 0, {0, 0}}};

} // namespace capsule::combine

namespace capsule::rules
{

int32_t combine_value(int32_t expr, const combine::ValueInputs& in)
{
   (void)expr;
   (void)in;
   return 0;
}

} // namespace capsule::rules
