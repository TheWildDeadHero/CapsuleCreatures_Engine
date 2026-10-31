/// @file
/// The engine's default growth tables (capsule/growth/data.h): no rules, forms, context values or training. A
/// build with battle data links its generated tables and functions (tools/battlec/growth.py, in the generated
/// battle.cpp) instead, which define every symbol of this file and so keep it out of the link.
#include "capsule/growth/data.h"

namespace capsule::growth
{

const GrowthData kGrowth = {nullptr, nullptr, nullptr, nullptr, 0, 0, 0, kNoRule, {0, 0}, monster::kNoRole, 0, 0,
                            0, {0, 0, {0}, 0, 0xFF}};

} // namespace capsule::growth

namespace capsule::rules
{

bool growth_when(int32_t rule, const growth::WhenInputs& in)
{
   (void)rule;
   (void)in;
   return false;
}

int32_t train_gain(int32_t yield, int32_t value, int32_t total, int32_t level)
{
   (void)yield;
   (void)value;
   (void)total;
   (void)level;
   return 0;
}

} // namespace capsule::rules
