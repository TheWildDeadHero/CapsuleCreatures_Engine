/// @file
/// The nursery script port of a program without battle data (capsule/script/nursery_port.h, B2): no nursery, no
/// eggs; every statement answers 0 at once.
/// C9: its definitions are weak (CAPSULE_WEAK): a game that defines its own port keeps it (capsule_game() links this
/// file for every game without eggs and a nursery).
#include "capsule/script/nursery_port.h"

#include "capsule/core/memory.h"

namespace capsule::script::nursery
{

CAPSULE_WEAK void attach(monster::Party* party)
{
   (void)party;
}

CAPSULE_WEAK void pick_open()
{
}

CAPSULE_WEAK void name_open(uint8_t member)
{
   (void)member;
}

CAPSULE_WEAK int16_t poll()
{
   return 0;
}

CAPSULE_WEAK int32_t query(NurseryOp kind, int32_t arg)
{
   (void)kind;
   (void)arg;
   return 0;
}

CAPSULE_WEAK int32_t withdraw(int32_t slot, int32_t money, int32_t& paid)
{
   (void)slot;
   (void)money;
   paid = 0;
   return 0;
}

} // namespace capsule::script::nursery
