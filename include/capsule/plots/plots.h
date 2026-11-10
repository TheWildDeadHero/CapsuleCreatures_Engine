/// @file
/// M6a K2 (B-5, CapsuleCreatures_Design.md): plots. One mechanism for soil, pots, static trees and Apricorns: a plot
/// grows a crop through stages on the **economy clock** (capsule/core/clock.h: real time, never Wait), and its state
/// is worked out from the time it was planted, never stored:
///   - growing: stage 0 to the type's stages - 1, a stage every crop.stage_hours;
///   - ripe (stage == the type's stages) for the type's ripe_hours (0: until picked); then the crop regrows from
///     stage 0 (a cycle), at most the type's regrow cycles, after which the plot is empty (withered);
///   - watering marks the current stage of the current cycle (a type with water);
///   - picking a ripe plot yields crop.yield_min plus (yield_max - yield_min) times the share of its stages watered
///     (a type without water: the minimum); the plot then empties, or is planted again (a type with replant: trees).
///
/// The world declares the plots (world.json "plots", tools/map_compiler.py): the types, the crops (each names an
/// item of the battle data), and the places (a map object each: the plot's id is its place's index). The overworld
/// shows a plot's object as its crop's sprite, animation = the stage (an empty plot: the type's sprite, or nothing),
/// through field::game::object_look().
///
/// Storage: the engine keeps a PlotRecord per place (Data::records, the save's Plots section). A world that declares
/// "storage": "game" keeps none: its game defines state(), crop(), yield(), plant(), water(), pick() and clear()
/// itself (the engine's are weak: a game that keeps its own records, M6a K3), and look() and the script's Plot
/// statements follow them.
#pragma once

#include "capsule/core/types.h"
#include "capsule/core/pod.h"

#include <cstdint>

namespace capsule::plots
{

/// @name Limits
///@{
inline constexpr uint8_t kMaxStages = 8;       ///< Stages a type has at most (a watering bit each).
inline constexpr uint8_t kStageEmpty = 0xFF;   ///< state() of an empty plot (or a withered one).
inline constexpr uint16_t kNoCrop = 0;         ///< PlotRecord::crop of an empty plot (crops count from 1).
inline constexpr uint16_t kNoPlot = 0xFFFF;    ///< plot_at() of an object that is no plot.
inline constexpr uint32_t kRecordBytes = 10;   ///< A record in the save (tools/save_layout.py mirrors it).
///@}

/// A plot type (world.json "plots" "types").
struct TypeData
{
   AssetKey sprite;         ///< What an empty plot shows (animation 0), or AssetKey{} (nothing).
   uint16_t ripe_hours;     ///< Hours a ripe crop waits before it regrows (0: until picked).
   uint8_t stages;          ///< Growing stages (1 to kMaxStages; the ripe stage comes after them).
   uint8_t regrow;          ///< Cycles a crop regrows before the plot empties (0: it empties after its first).
   uint8_t water;           ///< 1: watering counts toward the yield.
   uint8_t replant;         ///< 1: picking plants the crop again (a tree), instead of emptying the plot.
   uint8_t reserved[kPadAfterTwoBytes];   ///< Word padding.
};
CAPSULE_POD(TypeData, 12);

/// A crop (world.json "plots" "crops").
struct CropData
{
   AssetKey sprite;         ///< Its sprite: animation n is stage n, animation `stages` the ripe crop.
   uint16_t item;           ///< The item it yields (the battle data's id: index + 1).
   uint16_t stage_hours;    ///< Hours a stage takes (1 up).
   uint8_t type;            ///< Its plot type.
   uint8_t yield_min;       ///< Items it yields unwatered ...
   uint8_t yield_max;       ///< ... and watered every stage.
   uint8_t reserved[kPadAfterThreeBytes];   ///< Word padding.
};
CAPSULE_POD(CropData, 12);

/// A plot's place: a map object (world.json "plots" "places").
struct Place
{
   uint16_t map;            ///< The map (index into map::kWorld.maps).
   uint8_t local;           ///< The object's local ID.
   uint8_t type;            ///< The plot's type.
   uint16_t start_crop;     ///< The crop a new game plants there, ripe (crop index + 1), or kNoCrop.
   uint8_t reserved[kPadAfterTwoBytes];   ///< Word padding.
};
CAPSULE_POD(Place, 8);

/// A plot as the save keeps it (kRecordBytes in the save's Plots section; 12 in RAM).
struct PlotRecord
{
   uint32_t planted = 0;      ///< The economy clock when it was planted (seconds).
   uint16_t crop = kNoCrop;   ///< Its crop (index + 1), or kNoCrop.
   uint8_t watered = 0;       ///< Bit n: stage n was watered in cycle `cycle`.
   uint8_t cycle = 0;         ///< The cycle the watering bits belong to.
   uint8_t start_stage = 0;   ///< Stages it was planted ahead (a new game's ripe crops: the type's stages).
   uint8_t modifier = 0;      ///< Reserved (B-5's mulch-style modifiers; no rules yet).
};

/// The world's plots.
struct Data
{
   const TypeData* types;   ///< type_count types.
   const CropData* crops;   ///< crop_count crops.
   const Place* places;     ///< place_count places, sorted by map then local ID.
   uint8_t type_count;      ///< Types.
   uint8_t crop_count;      ///< Crops.
   uint16_t place_count;    ///< Places (plot ids 0 to place_count - 1).
   PlotRecord* records;     ///< The engine's records (place_count), or nullptr ("storage": "game").
};

/// The world's plots (link time: the generated world defines it; the engine's default, in src/save/save.cpp,
/// answers nullptr).
/// @return them, or nullptr when the world declares none
const Data* data();

/// A new game: every record empty, the places with a start crop planted ripe.
void begin_new();

/// A plot's state now.
/// @param id  the plot (its place's index)
/// @return its stage (0 up while growing, the type's stages when ripe), or kStageEmpty (empty, withered, no such
///         plot)
uint8_t state(uint16_t id);

/// The crop a plot holds.
/// @param id  the plot
/// @return crop index + 1, or kNoCrop (empty or withered)
uint16_t crop(uint16_t id);

/// M6a K3: the item of a plot's crop (follows crop(): a game's own storage too).
/// @param id  the plot
/// @return the item (the battle data's id), or 0 (empty)
uint16_t item(uint16_t id);

/// What picking a plot now would yield.
/// @param id  the plot
/// @return items (0 unless ripe)
uint8_t yield(uint16_t id);

/// Plants a crop.
/// @param id    the plot
/// @param crop  crop index + 1
/// @return false for an occupied plot, an unknown crop or one of another type
bool plant(uint16_t id, uint16_t crop);

/// Waters a growing plot (its current stage).
/// @param id  the plot
/// @return false unless it grows and its type counts watering
bool water(uint16_t id);

/// Picks a ripe plot: then it is empty, or planted again (a replant type).
/// @param id  the plot
/// @return the items it yields (0: not ripe, nothing changes)
uint8_t pick(uint16_t id);

/// Empties a plot.
/// @param id  the plot
void clear(uint16_t id);

/// The plot a map object is.
/// @param map    the map
/// @param local  the object's local ID
/// @return its id, or kNoPlot when it is none
uint16_t plot_at(uint16_t map, uint8_t local);

/// What a map object that is a plot shows (field::game::object_look(): the generated world of a world with plots).
/// @param map     the map
/// @param local   the object's local ID
/// @param sprite  its sprite (AssetKey{}: hidden)
/// @param anim    its animation
/// @return false when the object is no plot (it shows as its template says)
bool look(uint16_t map, uint8_t local, AssetKey& sprite, uint8_t& anim);

} // namespace capsule::plots
