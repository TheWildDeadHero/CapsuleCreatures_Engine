/// @file
/// Conditions (B-2, C-01e, D-23 sections 1.5 and 2): primary statuses, volatiles, weathers, side conditions and
/// field conditions as manifest records. A record keeps only what the engine itself needs: its names and texts
/// (presentation), its logic (an owner of BattleData::effects), and for a volatile its counter and word slots and
/// when its bits are cleared, for a side condition its layer cap. Everything a condition does is effect code in
/// its owner; the manifest's declarative fields (residual damage, lost turns, cures, durations, chains,
/// immunities, ...) are templates the battle compiler expands into handlers (tools/battlec/conditions.py).
///
/// Part of the battle data (capsule/battle/data.h, which includes every part).
#pragma once

#include "capsule/battle/data/common.h"
#include "capsule/text/encode.h"

namespace capsule::battle
{

/// @name Caps (D-23 section 2)
///@{
inline constexpr int kMaxStatuses = 63;          ///< Primary statuses (6-bit ids; index 0 is "none").
inline constexpr int kMaxStatusBits = 6;         ///< Widest status id in the PrimaryStatus role.
inline constexpr int kMaxVolatiles = 32;         ///< Volatiles (Battler::vol bits).
inline constexpr int kMaxVolatileCounters = 8;   ///< Volatile counter slots per battler (Battler::vcounter).
inline constexpr int kMaxVolatileWords = 4;      ///< Volatile word slots per battler (Battler::vword).
inline constexpr int kMaxBoundVolatiles = 4;     ///< Source slots per battler (Battler::vsource: bound volatiles, G1).
inline constexpr int kMaxSideConds = 16;         ///< Side conditions (SideState::cond bits).
inline constexpr int kMaxFieldConds = 16;        ///< Field conditions (FieldState::cond bits).
inline constexpr int kMaxMoveFlags = 16;         ///< Move flags (MoveData::flags bits; the manifest's move_flags).
inline constexpr int kMaxTags = 16;              ///< Tags (effect::Owner::tags bits; the manifest's tags).
///@}

/// @name Condition texts
/// An effect `text` in a status or weather owner that names one of its record's texts emits that text as the
/// engine's StatusText / WeatherText event (the kStatus* / kWeather* extra, capsule/battle/battle.h) instead of
/// an owner text: its text id is kConditionText plus the extra.
///@{
inline constexpr uint16_t kConditionText = 0x8000;       ///< Text id flag of a condition text.
inline constexpr uint16_t kConditionTextMask = 0x00FF;   ///< The extra in a condition text's id.
///@}

/// Volatile flag bits (VolatileData::flags; P-9 bit positions).
enum VolatileFlag : uint8_t
{
   VOLATILE_TURN,         ///< Its bit is cleared at the end of every turn (duration "turn").
   VOLATILE_SWITCH_OUT,   ///< Bit, counter and word are cleared when the holder leaves the field.
   VOLATILE_FAINT,        ///< Bit, counter and word are cleared when the holder faints.
   VOLATILE_PASSES,       ///< A passing switch (effect `switch ... pass`) hands it to the replacement.
   VOLATILE_HUD,          ///< Turning it on or off emits a Volatile event (presentation).
   VOLATILE_BOUND,        ///< Bound to its source (manifest `bound_to: "source"`, G1): it ends (volatile_off(),
                          ///< its end hook) when the battler that started it leaves the field or faints.
   NUM_VOLATILE_FLAGS     ///< Number of flags.
};

/// "No slot" of VolatileData::counter / word / source.
inline constexpr uint8_t kNoSlot = 0xFF;

/// A primary status (ROM, B-2): its texts and its logic.
struct StatusData
{
   const text::Code* name;       ///< Display name.
   const text::Code* inflicted;  ///< "{user} was ..." text.
   const text::Code* residual;   ///< Residual-damage text, or nullptr.
   const text::Code* skip;       ///< Lost-turn text, or nullptr.
   const text::Code* cured;      ///< Cured text.
   const text::Code* already;    ///< "{target} is already ..." text.
   uint16_t owner;               ///< Its logic in BattleData::effects (handlers, points, the expanded templates).
   uint8_t color;                ///< HUD color index (presentation).
   uint8_t reserved;             ///< Padding; 0.
};

/// Weather (ROM, C-01e battle part): the exclusive field condition (State::weather).
struct WeatherData
{
   const text::Code* name;       ///< Display name.
   const text::Code* started;    ///< Start text.
   const text::Code* continues;  ///< End-of-turn text.
   const text::Code* ended;      ///< End text.
   const text::Code* hurts;      ///< "{user} is hurt by ..." text, or nullptr.
   uint16_t owner;               ///< Its logic (point handlers, field_residual, the expanded templates).
   uint16_t reserved;            ///< Padding; 0.
};

/// A volatile (ROM, D-23 section 2): a bit of Battler::vol with an optional counter and word.
struct VolatileData
{
   const text::Code* name;   ///< Display name (HUD).
   uint16_t owner;           ///< Its logic in BattleData::effects.
   uint8_t counter;          ///< Its counter slot (Battler::vcounter), or kNoSlot.
   uint8_t word;             ///< Its word slot (Battler::vword), or kNoSlot.
   uint8_t flags;            ///< VolatileFlag bits.
   uint8_t source;           ///< Its source slot (Battler::vsource; VOLATILE_BOUND only), or kNoSlot.
   uint8_t reserved[kPadAfterTwoBytes];   ///< Padding; 0.
};

/// A side condition (ROM): a bit of SideState::cond with turns and layers.
struct SideCondData
{
   const text::Code* name;   ///< Display name (HUD).
   uint16_t owner;           ///< Its logic in BattleData::effects.
   uint8_t max_layers;       ///< Most layers (effect `side_on ... add_layer`; 1 = not layered).
   uint8_t reserved;         ///< Padding; 0.
};

/// A field condition (ROM): a bit of FieldState::cond with turns.
struct FieldCondData
{
   const text::Code* name;   ///< Display name (HUD).
   uint16_t owner;           ///< Its logic in BattleData::effects.
   uint16_t reserved;        ///< Padding; 0.
};

} // namespace capsule::battle
