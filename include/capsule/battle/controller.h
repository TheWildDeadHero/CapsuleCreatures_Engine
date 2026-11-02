/// @file
/// The controller channel (M5 BX): how a battle's logic hands work to its presentation and waits for it. The logic
/// writes a command into an endpoint's command buffer and marks the endpoint in the exec mask; the endpoint's
/// presentation (its controller: menus, text, animations, an input source or a remote side) runs the command over
/// as many frames as it needs, may write a reply into its reply buffer, and clears its bit when it is done. The
/// logic's script machine waits while the mask has bits set (capsule/battle/vm.h, wait_on()).
///
/// The exec mask is explicit: 32 bits in kLanes lanes of kLaneEndpoints bits, one bit per endpoint per lane
/// (bit lane * kLaneEndpoints + endpoint). Lane 0 is "running here"; the other lanes are the game's (for example a
/// command sent to, or acknowledged by, each side of a networked battle). The channel never interprets a command's
/// bytes beyond its first one, the command id that dispatch() uses.
///
/// Storage is the game's (Buffers<> or its own arrays): Channel is a view over it, so the game decides where the
/// buffers live (for example in its battle state, in external work RAM). Buffers keep their old bytes past what a
/// write covers, as a game whose replies are read past their written size expects.
///
/// Each frame the game runs every endpoint's current step function in endpoint order (run()); a controller's idle
/// step calls dispatch(), and a command's handler replaces the step with its own per-frame function until it calls
/// complete() and puts the idle step back.
#pragma once

#include <cstdint>

/// The controller channel.
namespace capsule::battle::controller
{

inline constexpr uint8_t kLaneEndpoints = 4;   ///< Endpoints (bits) per lane.
inline constexpr uint8_t kLanes = 8;           ///< Lanes of the 32-bit mask.
inline constexpr uint8_t kLocalLane = 0;       ///< The lane of commands running here.
inline constexpr uint8_t kNoEndpoint = 0xFF;   ///< No endpoint.

/// The exec mask.
using ExecFlags = uint32_t;

/// One endpoint's bit in a lane.
/// @param endpoint  0 to kLaneEndpoints - 1
/// @param lane      0 to kLanes - 1
/// @return the bit
constexpr ExecFlags bit(uint8_t endpoint, uint8_t lane = kLocalLane)
{
   return 1u << (lane * kLaneEndpoints + endpoint);
}

/// Every endpoint's bit of a lane.
/// @param lane  0 to kLanes - 1
/// @return the bits
constexpr ExecFlags lane_bits(uint8_t lane)
{
   return ((1u << kLaneEndpoints) - 1u) << (lane * kLaneEndpoints);
}

/// An endpoint's per-frame function (its controller's current state).
/// @param endpoint  the endpoint
/// @param context   the game's
using Step = void (*)(uint8_t endpoint, void* context);

/// A command's handler (the controller's function for one command id).
/// @param endpoint  the endpoint
/// @param context   the game's
using Handler = void (*)(uint8_t endpoint, void* context);

/// A controller's command set.
struct HandlerTable
{
   const Handler* handlers;   ///< Indexed by command id (nullptr entries: unknown).
   uint8_t count;             ///< Entries.
   Handler unknown;           ///< An id past the table or a nullptr entry, or nullptr: complete() at once.
};

/// A view over the game's buffers.
struct Channel
{
   uint8_t* commands;       ///< endpoints x buffer_bytes: logic to presentation.
   uint8_t* replies;        ///< endpoints x buffer_bytes: presentation to logic.
   ExecFlags* exec;         ///< The exec mask.
   Step* steps;             ///< endpoints entries: each endpoint's current step (nullptr: none).
   uint16_t buffer_bytes;   ///< Bytes of one buffer.
   uint8_t endpoints;       ///< Endpoints (at most kLaneEndpoints).
};

/// Fixed storage for a channel (a game may keep its own arrays instead).
/// @tparam Endpoints    endpoints (at most kLaneEndpoints)
/// @tparam BufferBytes  bytes of one buffer
template <uint8_t Endpoints, uint16_t BufferBytes>
struct Buffers
{
   static_assert(Endpoints > 0 && Endpoints <= kLaneEndpoints, "one lane holds the endpoints");

   uint8_t commands[Endpoints][BufferBytes];   ///< Command buffers.
   uint8_t replies[Endpoints][BufferBytes];    ///< Reply buffers.
   ExecFlags exec;                             ///< The exec mask.
   Step steps[Endpoints];                      ///< Current steps.

   /// The view.
   /// @return it
   Channel view()
   {
      return Channel{&commands[0][0], &replies[0][0], &exec, steps, BufferBytes, Endpoints};
   }
};

/// An endpoint's command buffer.
/// @param ch        the channel
/// @param endpoint  the endpoint
/// @return its bytes (buffer_bytes)
uint8_t* command(const Channel& ch, uint8_t endpoint);

/// An endpoint's reply buffer.
/// @param ch        the channel
/// @param endpoint  the endpoint
/// @return its bytes
uint8_t* reply(const Channel& ch, uint8_t endpoint);

/// Copies bytes into the start of an endpoint's command buffer (the rest keeps its bytes); the mask is not changed.
/// @param ch        the channel
/// @param endpoint  the endpoint
/// @param data      the bytes
/// @param size      their count (clipped to buffer_bytes)
void write_command(const Channel& ch, uint8_t endpoint, const uint8_t* data, uint16_t size);

/// Copies bytes into the start of an endpoint's reply buffer (the rest keeps its bytes).
/// @param ch        the channel
/// @param endpoint  the endpoint
/// @param data      the bytes
/// @param size      their count (clipped to buffer_bytes)
void write_reply(const Channel& ch, uint8_t endpoint, const uint8_t* data, uint16_t size);

/// Sets an endpoint's bit in a lane.
/// @param ch        the channel
/// @param endpoint  the endpoint
/// @param lane      the lane
void mark(const Channel& ch, uint8_t endpoint, uint8_t lane = kLocalLane);

/// Clears an endpoint's bit in a lane (its command is done).
/// @param ch        the channel
/// @param endpoint  the endpoint
/// @param lane      the lane
void complete(const Channel& ch, uint8_t endpoint, uint8_t lane = kLocalLane);

/// write_command() and mark() in the local lane.
/// @param ch        the channel
/// @param endpoint  the endpoint
/// @param data      the bytes
/// @param size      their count
void send(const Channel& ch, uint8_t endpoint, const uint8_t* data, uint16_t size);

/// Whether an endpoint's bit is set in a lane.
/// @param ch        the channel
/// @param endpoint  the endpoint
/// @param lane      the lane
/// @return true while it is
bool pending(const Channel& ch, uint8_t endpoint, uint8_t lane = kLocalLane);

/// Whether any of the given bits is set.
/// @param ch    the channel
/// @param mask  the bits (all: anything running or in flight)
/// @return true while one is
bool busy(const Channel& ch, ExecFlags mask = ~0u);

/// Sets an endpoint's current step.
/// @param ch        the channel
/// @param endpoint  the endpoint
/// @param step      its function (nullptr: none)
void set_step(const Channel& ch, uint8_t endpoint, Step step);

/// Runs the handler of an endpoint's pending command (the idle step's work): nothing when its local bit is clear;
/// an unknown id runs the table's unknown handler, or completes at once.
/// @param ch        the channel
/// @param endpoint  the endpoint
/// @param table     its controller's handlers
/// @param context   passed to the handler
/// @return true when a command was pending
bool dispatch(const Channel& ch, uint8_t endpoint, const HandlerTable& table, void* context);

/// Runs the first @p count endpoints' steps once, in endpoint order (one frame of the presentation side).
/// @param ch       the channel
/// @param count    endpoints to run (clipped to endpoints)
/// @param context  passed to the steps
/// @param active   receives each endpoint before its step runs (the game's "current endpoint"), or nullptr
void run(const Channel& ch, uint8_t count, void* context, uint8_t* active = nullptr);

/// Clears the mask and the steps (the buffers keep their bytes).
/// @param ch  the channel
void reset(const Channel& ch);

/// Zeroes every buffer (a game that clears them at a battle's start).
/// @param ch  the channel
void clear_buffers(const Channel& ch);

}   // namespace capsule::battle::controller
