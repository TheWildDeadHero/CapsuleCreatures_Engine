/// @file
/// The controller channel (capsule/battle/controller.h).
#include "capsule/battle/controller.h"

namespace capsule::battle::controller
{
namespace
{

/// Copies @p size bytes (clipped to the buffer) into a buffer's start.
/// @param dst     the buffer
/// @param room    its bytes
/// @param data    the bytes
/// @param size    their count
void copy_in(uint8_t* dst, uint16_t room, const uint8_t* data, uint16_t size)
{
   const uint16_t n = size < room ? size : room;
   for (uint16_t i = 0; i < n; ++i)
   {
      dst[i] = data[i];
   }
}

} // namespace

uint8_t* command(const Channel& ch, uint8_t endpoint)
{
   return ch.commands + static_cast<uint32_t>(endpoint) * ch.buffer_bytes;
}

uint8_t* reply(const Channel& ch, uint8_t endpoint)
{
   return ch.replies + static_cast<uint32_t>(endpoint) * ch.buffer_bytes;
}

void write_command(const Channel& ch, uint8_t endpoint, const uint8_t* data, uint16_t size)
{
   if (endpoint < ch.endpoints)
   {
      copy_in(command(ch, endpoint), ch.buffer_bytes, data, size);
   }
}

void write_reply(const Channel& ch, uint8_t endpoint, const uint8_t* data, uint16_t size)
{
   if (endpoint < ch.endpoints)
   {
      copy_in(reply(ch, endpoint), ch.buffer_bytes, data, size);
   }
}

void mark(const Channel& ch, uint8_t endpoint, uint8_t lane)
{
   *ch.exec |= bit(endpoint, lane);
}

void complete(const Channel& ch, uint8_t endpoint, uint8_t lane)
{
   *ch.exec &= ~bit(endpoint, lane);
}

void send(const Channel& ch, uint8_t endpoint, const uint8_t* data, uint16_t size)
{
   write_command(ch, endpoint, data, size);
   mark(ch, endpoint);
}

bool pending(const Channel& ch, uint8_t endpoint, uint8_t lane)
{
   return (*ch.exec & bit(endpoint, lane)) != 0;
}

bool busy(const Channel& ch, ExecFlags mask)
{
   return (*ch.exec & mask) != 0;
}

void set_step(const Channel& ch, uint8_t endpoint, Step step)
{
   if (endpoint < ch.endpoints)
   {
      ch.steps[endpoint] = step;
   }
}

bool dispatch(const Channel& ch, uint8_t endpoint, const HandlerTable& table, void* context)
{
   if (endpoint >= ch.endpoints || !pending(ch, endpoint))
   {
      return false;
   }
   const uint8_t id = command(ch, endpoint)[0];
   const Handler h = id < table.count ? table.handlers[id] : nullptr;
   if (h != nullptr)
   {
      h(endpoint, context);
   }
   else if (table.unknown != nullptr)
   {
      table.unknown(endpoint, context);
   }
   else
   {
      complete(ch, endpoint);
   }
   return true;
}

void run(const Channel& ch, uint8_t count, void* context, uint8_t* active)
{
   const uint8_t n = count < ch.endpoints ? count : ch.endpoints;
   for (uint8_t e = 0; e < n; ++e)
   {
      if (active != nullptr)
      {
         *active = e;
      }
      if (ch.steps[e] != nullptr)
      {
         ch.steps[e](e, context);
      }
   }
   if (active != nullptr)
   {
      *active = n;   // as a loop over the current endpoint leaves it
   }
}

void reset(const Channel& ch)
{
   *ch.exec = 0;
   for (uint8_t e = 0; e < ch.endpoints; ++e)
   {
      ch.steps[e] = nullptr;
   }
}

void clear_buffers(const Channel& ch)
{
   const uint32_t bytes = static_cast<uint32_t>(ch.endpoints) * ch.buffer_bytes;
   for (uint32_t i = 0; i < bytes; ++i)
   {
      ch.commands[i] = 0;
      ch.replies[i] = 0;
   }
}

}   // namespace capsule::battle::controller
