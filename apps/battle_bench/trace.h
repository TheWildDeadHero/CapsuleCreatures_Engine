/// @file
/// The bench's traces (D-23 section 3.3): every draw from the battle stream ("rng" records) and every call the
/// effect VM makes into its host ("vm" records: owner, hook, program, pc and the source position from the debug line
/// table).
///
/// Both are link-time seams, like the engine's other replaceable parts. The bench builds the chosen generator with
/// its next16() renamed (cmake/parts/bench.cmake) and defines capsule::rng::next16() here, which calls it and
/// records the draw; it builds src/battle/effect_host.cpp with its host functions renamed and defines
/// effect::host_get() ... host_action() here, which record the call and forward. The engine sources are unchanged.
#pragma once

#include "capsule/battle/battle.h"

#include <cstdint>

namespace capsule::bench
{

/// What the run traces.
struct TraceOptions
{
   bool rng = false;      ///< "rng" records.
   bool vm = false;       ///< "vm" records.
   bool events = false;   ///< "event" records (every battle event).
};

/// Starts tracing a battle (call before start()).
/// @param s     the battle
/// @param opts  what to trace
void trace_start(const battle::State& s, const TraceOptions& opts);

/// Draws the battle stream gave so far.
/// @return count
uint32_t draw_count();

/// The source position of an instruction.
/// @param program  program index
/// @param pc       instruction offset in it
/// @return the entry, or nullptr when the table has none
const battle::effect::SourceLine* source_of(uint16_t program, uint16_t pc);

/// Writes an "event" record (transcript::EventObserver).
/// @param s     battle
/// @param e     the event
/// @param user  unused
void trace_event(const battle::State& s, const battle::Event& e, void* user);

/// A phase's name.
/// @param p  phase
/// @return name
const char* phase_name(battle::Phase p);

} // namespace capsule::bench
