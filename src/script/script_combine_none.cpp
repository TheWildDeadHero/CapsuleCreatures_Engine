/// @file
/// The combine script port of a program without battle data (capsule/script/combine_port.h, B1): nothing is attached
/// and nothing ever combines (`combine open|run ...` gives kCombineBack at once, the queries 0).
/// C9: its definitions are weak (CAPSULE_WEAK): a game that defines its own port keeps it (capsule_game() links this
/// file for every game without combination methods).
#include "capsule/script/combine_port.h"

#include "capsule/core/memory.h"

namespace capsule::script::combine
{

CAPSULE_WEAK void attach(battle::State* lent, monster::Party* party, storage::Storage* boxes, rng::Stream* rng)
{
   (void)lent;
   (void)party;
   (void)boxes;
   (void)rng;
}

CAPSULE_WEAK void set_menus(bool on)
{
   (void)on;
}

CAPSULE_WEAK void open(uint8_t method)
{
   (void)method;
}

CAPSULE_WEAK void run(uint8_t method, const uint8_t* slots, uint8_t count)
{
   (void)method;
   (void)slots;
   (void)count;
}

CAPSULE_WEAK int16_t poll()
{
   return kCombineBack;
}

CAPSULE_WEAK int32_t query(CombineOp kind, uint8_t method, const uint8_t* slots, uint8_t count)
{
   (void)kind;
   (void)method;
   (void)slots;
   (void)count;
   return 0;
}

} // namespace capsule::script::combine
