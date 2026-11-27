/// @file
/// D-36 §3.1 G3: the Dex statement's port of a program without the engine library's records (the tests that build
/// the script VM themselves): every answer 0. Weak, like the engine library's src/script/script_dex.cpp.
#include "capsule/core/memory.h"
#include "capsule/script/port.h"

namespace capsule::script::dex
{

CAPSULE_WEAK int32_t count(bool caught, bool regional)
{
   (void)caught;
   (void)regional;
   return 0;
}

CAPSULE_WEAK int32_t species(bool caught, int32_t id)
{
   (void)caught;
   (void)id;
   return 0;
}

CAPSULE_WEAK int32_t complete(bool regional)
{
   (void)regional;
   return 0;
}

CAPSULE_WEAK int32_t national()
{
   return 0;
}

CAPSULE_WEAK void enable_national()
{
}

} // namespace capsule::script::dex
