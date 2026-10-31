/// @file
/// A `capsule.bench/1` scenario (D-23 section 3.3) read into what the run needs: the battle setup with its party
/// records, the battler pokes applied after start(), the scripted choices and answers, the stop condition and the
/// trace options. Names are the manifest's (tools/bench_scenario.py documents the format and validates it first).
#pragma once

#include "json.h"
#include "trace.h"

#include "support/battle_transcript.h"

#include <cstdint>
#include <string>
#include <vector>

namespace capsule::bench
{

/// One scripted choice; a move may be named instead of given by slot.
struct Pick
{
   battle::transcript::Choice choice;   ///< The choice (slot when by slot).
   uint16_t move;                       ///< Move by name (resolved to its slot when chosen), or kNone16.
};

/// The choices of one turn: one for every human battler, or one per field position (the last repeats).
using TurnPicks = std::vector<Pick>;

/// One scripted answer to an effect request.
struct Answer
{
   battle::RequestKind kind;   ///< The request it answers.
   int32_t value;              ///< Move slot (LearnMove), option (Choice) or party member (SwitchTarget).
};

/// One resource value a member or a poke sets (D-25; "resources": {name: value, or [value per move slot]}).
struct ResourcePoke
{
   uint8_t resource;   ///< The resource (BattleData::resources index).
   int8_t slot;        ///< Move slot (move resources), or -1.
   uint16_t value;     ///< The value (clamped to the pool's maximum when a poke is applied).
};

/// A battler poke (applied after start()); a field is applied when its PokeField bit is in Poke::fields.
struct Poke
{
   uint8_t party;            ///< Participant index.
   uint8_t member;           ///< Member index in its party.
   uint32_t fields;          ///< PokeField bits.
   uint16_t hp;              ///< Current HP.
   uint8_t status;           ///< Status index (0 = none).
   uint8_t status_counter;   ///< Battler::status_counter ("status_counter").
   uint8_t status_aux;       ///< Battler::status_aux ("status_aux").
   uint8_t counters[battle::kMaxVolatileCounters];   ///< Volatile counters (those whose bit is in counter_mask).
   uint8_t counter_mask;     ///< Counter slots given ("counters": {volatile: n} sets that volatile's counter).
   uint16_t item;            ///< Held item (0 = none).
   uint32_t volatiles;       ///< Volatile bits.
   int8_t stages[battle::kMaxStages];   ///< Stages (those whose bit is in stage_mask).
   uint64_t stage_mask;      ///< Stages given (bit = stage index; kMaxStages can pass 32).
   std::vector<ResourcePoke> resources;   ///< Resource pools ("resources"), in order.
};
static_assert(battle::kMaxStages <= 64, "Poke::stage_mask holds a bit per stage");

/// Battler fields a poke sets (bit numbers in Poke::fields).
enum PokeField : uint8_t
{
   POKE_HP,             ///< Poke::hp.
   POKE_STATUS,         ///< Poke::status.
   POKE_STATUS_COUNTER, ///< Poke::status_counter.
   POKE_STATUS_AUX,     ///< Poke::status_aux.
   POKE_ITEM,           ///< Poke::item.
   POKE_VOLATILES,      ///< Poke::volatiles.
   NUM_POKE_FIELDS      ///< Number of fields.
};

/// When the run stops.
enum class StopKind : uint8_t
{
   End,      ///< At the battle's end (or the step limit).
   Turns,    ///< When turn Scenario::stop_value + 1 would start.
   Steps,    ///< After Scenario::stop_value step() calls.
   NUM_KINDS ///< Number of kinds.
};

/// A read scenario.
struct Scenario
{
   std::string name;                       ///< Its name (the transcript's first line).
   battle::Setup setup;                    ///< The battle's setup (participants point into the party records).
   bool weather_turns_given;               ///< Whether weather_turns is set.
   uint8_t weather_turns;                  ///< Weather turns after start() (field.weather_turns).
   std::vector<Poke> pokes;                ///< Battler pokes.
   std::vector<TurnPicks> choices;         ///< Per turn (the last repeats; none: the first usable move).
   std::vector<Answer> answers;            ///< In order.
   StopKind stop;                          ///< Stop condition.
   int stop_value;                         ///< Its turns or steps.
   int step_limit;                         ///< Most step() calls.
   TraceOptions trace;                     ///< Traces.
   bool owner_texts;                       ///< The transcript shows owner texts and condition changes (E13b).
};

/// Reads a scenario document; writes an "error" record for each problem found.
/// @param doc      the parsed document
/// @param parties  party records it fills (battle::kMaxParties entries)
/// @param out      the scenario
/// @return false when it has errors
bool load_scenario(const json::Value& doc, monster::Party* parties, Scenario& out);

/// Applies the battler pokes and the field settings (after start()).
/// @param s   the battle
/// @param sc  the scenario
/// @return false when a poke names a member the battle does not have (error written)
bool apply_pokes(battle::State& s, const Scenario& sc);

} // namespace capsule::bench
