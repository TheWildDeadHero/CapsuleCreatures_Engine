/// @file
/// Empty script bank, linked when a game has no scripts (the generated bank
/// replaces it).
#include "capsule/script/data.h"

namespace capsule::script
{

const Bank kBank = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {},      {},      {},
                    {},      {},      0,       0,       0,       1,       0,       nullptr, nullptr, nullptr,
                    nullptr, nullptr, 0,       0,       {},      nullptr, nullptr, nullptr, 0};

} // namespace capsule::script
