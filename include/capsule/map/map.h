/// @file
/// Overworld maps (C-01b, C-03a/b/c, A-9, E-7): compiled map tables, cell
/// queries and the chunk-streamed background renderer.
///
/// Units: a metatile is 16x16 pixels (2x2 tiles); a chunk is 8x8 metatiles
/// (128x128 pixels). A map is a sparse set of chunks found through a dense
/// grid; everything outside them, and every unpainted cell, is the map's
/// border pattern, which is always impassable (C-03b).
///
/// Layers (C-01b): a map has up to four **tile layers**, each on a BG layer
/// of its choice (BG0-BG3) with its own draw priority. A metatile is one
/// layer of 2x2 tiles, graphics only. Every cell has one metatile per tile
/// layer (or none) and one **attribute cell** (MapCell): behavior,
/// environment tag, collision, elevation, the encounter flag and the
/// encounter slot, painted on their own layer. A cell whose attributes are
/// unpainted shows the border.
///
/// VRAM plan while a map is shown:
/// | What | Where |
/// |---|---|
/// | map tiles, 4bpp, up to kMaxTiles | character blocks 0-1 (tile 0 is transparent) |
/// | tile layer on BGn | screen block 28 + (n + 3) % 4 (BG1 28, BG2 29, BG3 30, BG0 31), 32x32 ring |
/// | BGs without a tile layer | hidden at enter(); BG0, if free, moves to character block 2 and screen block 31 for text and the text box (gfx::background's map plan) |
/// | palettes | banks 0 to kMaxPalettes-1; entry 0 of bank 0 = backdrop |
///
/// Events (B-3): a map's warps, triggers and signs are bucketed per chunk:
/// the chunk's directory entry names its first event and count, and each
/// event holds its chunk-local cell, so a step looks only at the events of
/// one chunk (find_event()). Warps name their landing cell, resolved by the
/// compiler. Each map also names its area root (B-2: the outdoor map whose
/// area it is in), and the world lists the step dispatcher's hooks.
///
/// Objects (B-21, D-17): each map lists up to kMaxObjects object templates
/// (NPCs, spotters, pickups; their local ID is the list index), their
/// event pages and their move routes; capsule::field spawns and moves them.
/// Each map may name a location (its name banner and preview, D-17).
///
/// Sprites go between layers by priority: the walker's priority comes from
/// the world's elevation table (World::elevations), so a layer drawn at priority 1 covers a
/// walker at priority 2.
///
/// Rendering: each tile layer's 32x32-tile screen is a ring holding 16x16
/// metatiles around the camera; moving the camera redraws only the metatile
/// rows and columns that came into view. The rings are drawn into a mirror
/// in EWRAM and their changed rows reach VRAM through the upload queue
/// (must-land, A-5), so they land in the same VBlank as the new scroll
/// offsets. Tile animations replace runs of tiles on their period (staggered
/// by the compiler), queued as streaming uploads. Map tiles themselves load
/// immediately in enter(), which belongs to a scene change (faded or blank
/// screen).
#pragma once

#include "capsule/core/pod.h"
#include "capsule/core/types.h"
#include "capsule/encounter/data.h"
#include "capsule/hal/video.h"

#include <cstdint>

/// Overworld maps.
namespace capsule::map
{

using hal::video::Color;

/// One attribute cell: behavior, environment tag, collision, elevation,
/// encounter flag and encounter slot (C-03c, D-12).
using MapCell = uint32_t;

inline constexpr MapCell kCellBehaviorMask = 0x000000FF;   ///< Bits 0-7: index into World::behaviors.
inline constexpr MapCell kCellEnvMask = 0x0000FF00;   ///< Bits 8-15: free tag for game data.
inline constexpr MapCell kCellCollisionMask = 0x00030000;   ///< Bits 16-17: collision (0 = passable).
inline constexpr MapCell kCellElevationMask = 0x003C0000;   ///< Bits 18-21: elevation.
inline constexpr MapCell kCellEncounter = 0x00400000;   ///< Bit 22: stepping here may start an encounter.
inline constexpr MapCell kCellEncounterSlotMask = 0x07800000;   ///< Bits 23-26: the map's encounter slot (0-15).
inline constexpr int kCellEnvShift = 8;   ///< Shift of the environment tag.
inline constexpr int kCellCollisionShift = 16;   ///< Shift of the collision bits.
inline constexpr int kCellElevationShift = 18;   ///< Shift of the elevation bits.
inline constexpr uint8_t kElevationMask =
   15;   ///< An elevation value's bits (0-15; World::elevations has 16 entries).
inline constexpr int kCellEncounterSlotShift = 23;   ///< Shift of the encounter slot.
inline constexpr MapCell kCellUndefined = 0xFFFFFFFF;   ///< An unpainted cell (the border shows).
inline constexpr MapCell kCellBorder = kCellCollisionMask;   ///< What border cells read as: impassable, behavior 0.

/// A metatile id in a tile layer.
using MetatileId = uint16_t;
inline constexpr MetatileId kNoMetatile = 0xFFFF;   ///< Nothing on this layer here (transparent).

inline constexpr int kChunkMetatiles = 8;   ///< Metatiles per chunk side.
inline constexpr int kChunkCells = 64;   ///< Cells per chunk.
inline constexpr int kMetatilePixels = 16;   ///< Pixels per metatile side.
inline constexpr int kChunkPixels = 128;   ///< Pixels per chunk side.
inline constexpr int kMaxLayers = 4;   ///< Tile layers per map (one per BG at most).
inline constexpr int kMaxTiles = 1024;   ///< 4bpp tiles a map may load (character blocks 0-1).
inline constexpr int kMaxPalettes = 13;   ///< BG palette banks a map may use (13-15 are the UI's).
inline constexpr int kMaxAnimations = 32;   ///< Tile animations per map (I2: 32, was 16).
inline constexpr int kRingMetatiles = 16;   ///< Metatiles the 32x32-tile screen ring holds per side.

/// @name VRAM plan
///@{
inline constexpr uint8_t kTileCharBlock = 0;   ///< Character block of map tile 0 (tiles run into block 1).
inline constexpr uint8_t kFirstScreenBlock = 28;   ///< BG1's ring; see screen_block_of().
inline constexpr uint8_t kWalkerPriority = 2;   ///< Default sprite priority (no elevation table).
inline constexpr int kRingBgOffset = 3;   ///< Added to a BG index, mod 4, so BG1's ring is the first.
inline constexpr int kMetatileEntries = 4;   ///< Map entries in one metatile layer (2x2).
///@}

/// Screen block of a tile layer's ring.
/// @param bg  0-3
/// @return 28 + (bg + 3) % 4: BG1 28, BG2 29, BG3 30, BG0 31
constexpr uint8_t screen_block_of(uint8_t bg)
{
   return static_cast<uint8_t>(kFirstScreenBlock + (bg + kRingBgOffset) % hal::video::kNumBgs);
}

/// Behavior index of a cell.
/// @param c  the cell
/// @return index into World::behaviors
constexpr uint8_t cell_behavior(MapCell c)
{
   return static_cast<uint8_t>(c & kCellBehaviorMask);
}

/// Environment tag of a cell.
/// @param c  the cell
/// @return 0-255
constexpr uint8_t cell_env(MapCell c)
{
   return static_cast<uint8_t>((c & kCellEnvMask) >> kCellEnvShift);
}

/// Collision bits of a cell.
/// @param c  the cell
/// @return 0 (passable) to 3
constexpr uint8_t cell_collision(MapCell c)
{
   return static_cast<uint8_t>((c & kCellCollisionMask) >> kCellCollisionShift);
}

/// Elevation of a cell.
/// @param c  the cell
/// @return 0-15 (what a level meets comes from World::elevations)
constexpr uint8_t cell_elevation(MapCell c)
{
   return static_cast<uint8_t>((c & kCellElevationMask) >> kCellElevationShift);
}

/// Whether stepping on a cell may start an encounter.
/// @param c  the cell
/// @return the encounter flag
constexpr bool cell_encounter(MapCell c)
{
   return (c & kCellEncounter) != 0;
}

/// Which of the map's encounter slots a cell uses (encounter::Ref::slot).
/// @param c  the cell
/// @return 0-15
constexpr uint8_t cell_encounter_slot(MapCell c)
{
   return static_cast<uint8_t>((c & kCellEncounterSlotMask) >> kCellEncounterSlotShift);
}

/// A metatile: one layer of 2x2 background map entries (ROM), in the order
/// top-left, top-right, bottom-left, bottom-right; tile indices count from
/// the map's tile 0, flips and palette bank included.
struct Metatile
{
   uint16_t tiles[kMetatileEntries];   ///< Map entries.
};
CAPSULE_POD(Metatile, 8);

/// One tile layer of a map (ROM).
struct TileLayer
{
   uint8_t bg;   ///< BG layer, 0-3.
   uint8_t priority;   ///< Hardware draw priority, 0 (front) to 3.
   uint8_t reserved[kPadAfterTwoBytes];   ///< 0.
};
CAPSULE_POD(TileLayer, 4);

/// How a behavior moves a walker (E-7).
enum class BehaviorKind : uint8_t
{
   None,   ///< Nothing special.
   Slide,   ///< Ice: keep moving the same way until blocked.
   Ledge,   ///< Jump over it moving in its direction; a wall otherwise.
   Push,   ///< Forced step in its direction.
   Counter,   ///< I3: a counter: pressing A facing it talks to the map object on the cell beyond (shop counters).
   NUM_BEHAVIOR_KINDS   ///< Number of kinds.
};

/// A terrain class (ROM, E-7). Behaviors are data: the names and their
/// meaning come from the world manifest (P-3).
struct Behavior
{
   uint8_t passable;   ///< Vehicle classes that may enter (bit n = class n; class 0 = on foot).
   BehaviorKind kind;   ///< Movement effect.
   uint8_t direction;   ///< For Ledge and Push: field::Direction.
   uint8_t walls;   ///< I2: the cell's sides that are walls, bit d = field::Direction d (one-way fences): a step
                    ///< leaving the cell through a wall side, or entering it through one, is blocked. 0: none.
};
CAPSULE_POD(Behavior, 4);

/// I2: whether walls stop a step (Behavior::walls): the cell it leaves has a wall on the side it moves through,
/// or the cell it enters has one on the side it comes in by.
/// @param from  the behavior of the cell left
/// @param to    the behavior of the cell entered
/// @param d     the step's field::Direction (down 0, up 1, left 2, right 3: d ^ 1 is the opposite side)
/// @return true when the step is blocked
constexpr bool walls_block(const Behavior& from, const Behavior& to, uint8_t d)
{
   constexpr uint8_t kOppositeSide = 1;   // down / up and left / right differ in bit 0
   return ((from.walls >> d) & 1u) != 0 || ((to.walls >> (d ^ kOppositeSide)) & 1u) != 0;
}

/// @name Elevations (A1, D-22 decision 5)
/// Levels are data: the world manifest's `elevations` give each of the 16
/// levels the levels it meets, what arriving on it does and a sprite
/// priority. Two levels meet when either one's meet mask holds the other;
/// a mover may enter a cell when they meet, except that a mover standing on
/// a "keep" level (only data puts it there) meets by the cell's mask alone.
/// Without a table (World::elevations nullptr) the default rule applies:
/// level 0 meets every level, 15 meets every level and keeps the mover's
/// (bridges), 1-14 meet themselves; every level takes kWalkerPriority.
///@{
inline constexpr int kElevationLevels = 16;   ///< Levels of World::elevations (a cell's 4 bits).
inline constexpr uint16_t kMeetsAll = 0xFFFF;   ///< Elevation::meets: every level.
inline constexpr uint8_t kElevationAny = 0;   ///< Default rule: the level that meets every level (stairs).
inline constexpr uint8_t kElevationMulti = 15;   ///< Default rule: the level that meets every level and keeps
                                                 ///< the mover's (bridges).
inline constexpr uint8_t kElevationKeep = 1;   ///< Elevation::flags: arriving keeps the mover's level.
///@}

/// One level of the world's elevation table (ROM, A1).
struct Elevation
{
   uint16_t meets;   ///< Levels it meets: bit n = level n (its own bit included).
   uint8_t flags;   ///< kElevationKeep.
   uint8_t priority;   ///< Sprite priority of a walker or object on it, 0 (front) to 3.
};
CAPSULE_POD(Elevation, 4);

/// The default rule's entry for a level (see "Elevations").
/// @param level  0-15
/// @return its entry
constexpr Elevation default_elevation(uint8_t level)
{
   if (level == kElevationAny)
   {
      return Elevation{kMeetsAll, 0, kWalkerPriority};
   }
   if (level == kElevationMulti)
   {
      return Elevation{kMeetsAll, kElevationKeep, kWalkerPriority};
   }
   return Elevation{static_cast<uint16_t>(1u << level), 0, kWalkerPriority};
}

/// @name Vehicle classes (A2, D-22 decision 5)
/// A vehicle class is a movement mode (on foot, swimming, riding...): its
/// walking and running speeds, whether it may run, the first animation of
/// its sprite set and (through Behavior::passable) the terrain it enters.
/// Class 0 is on foot. Without a table (World::vehicle_classes nullptr)
/// every class walks and runs as on foot.
///@{
inline constexpr uint8_t kDefaultWalkSpeed = 1;   ///< Pixels per frame walking (class 0's default).
inline constexpr uint8_t kDefaultRunSpeed = 2;   ///< Pixels per frame running (class 0's default).
inline constexpr uint8_t kVehicleCanRun = 1;   ///< VehicleClass::flags: the class may run.
inline constexpr uint8_t kMaxVehicleClasses = 8;   ///< Classes per world (Behavior::passable has 8 bits).
///@}

/// A movement mode (ROM, A2).
struct VehicleClass
{
   uint8_t walk_speed;   ///< Pixels per frame walking (1-8, a divisor of a cell's 16).
   uint8_t run_speed;   ///< Pixels per frame running.
   uint8_t first_animation;   ///< Sprite animation its set starts at (capsule/field/field.h convention:
                              ///< + 0-3 standing, + 4-7 walking, + 8-11 running, + 12-15 turning).
   uint8_t flags;   ///< kVehicleCanRun, kVehicleDismount, kVehicleBumps, kVehicleKeepsMoving.
   uint8_t turn_frames = 0;   ///< I3: frames a turn in place takes (0: field::kTurnFrames).
   uint8_t reserved[kPadAfterOneByte] = {};   ///< 0.
};
CAPSULE_POD(VehicleClass, 8);

/// Class 0's default (today's walk / run).
inline constexpr VehicleClass kDefaultVehicle{kDefaultWalkSpeed, kDefaultRunSpeed, 0, kVehicleCanRun, 0, {0, 0, 0}};

/// VehicleClass::flags (F2): stepping from this class onto a cell only class 0 (on foot) may enter changes the
/// walker to class 0 and takes the step (leaving the water after crossing it; world manifest `"dismount": true`).
inline constexpr uint8_t kVehicleDismount = 2;

/// VehicleClass::flags (I3): a step into something that blocks walks in place for one step's time (a bump; Gen III)
/// instead of standing.
inline constexpr uint8_t kVehicleBumps = 4;

/// VehicleClass::flags (I3): a walker that arrives on a cell with another direction held steps on in it without
/// turning in place first (Gen III: a turn happens only from standing).
inline constexpr uint8_t kVehicleKeepsMoving = 8;

/// @name Map::flags bits
///@{
inline constexpr uint8_t kMapNoRun = 1;   ///< Running is not allowed on the map (map manifest `can_run`: false).
inline constexpr uint8_t kMapEscapePoint = 2;   ///< F2: warps leaving it for a map without this bit record the
                                                ///< escape point (map manifest `escape_point`: true; outdoors).
///@}

/// One populated chunk (ROM, C-03c / GAP-8). Laid out without packing, so
/// the GBA reads it with aligned loads.
struct ChunkDirectoryEntry
{
   int16_t chunk_x;   ///< Chunk column.
   int16_t chunk_y;   ///< Chunk row.
   uint32_t cells_offset;   ///< Index of its 64 attribute cells in Map::cells.
   uint32_t layers_offset;   ///< Index of its first layer's 64 metatile ids in Map::layer_cells
      ///< (layer k follows at + 64 k).
   uint16_t encounter_layer;   ///< Encounter layer index, 0xFFFF = none (C-03c; not used yet).
   uint16_t palette_set;   ///< Chunk palette set, 0xFFFF = map palettes (C-02c; not used yet).
   uint8_t zone;   ///< Zone within the map (GAP-8; not used yet).
   uint8_t event_count;   ///< Events in this chunk (B-3).
   uint16_t event_first;   ///< Index of its first event in Map::events.
   uint16_t reserved;   ///< 0.
};
CAPSULE_POD(ChunkDirectoryEntry, 24);

/// What a map event does (B-3).
enum class EventKind : uint8_t
{
   Warp,   ///< Moves the walker to another map's warp.
   Trigger,   ///< Runs a script when the walker arrives on the cell.
   Sign,   ///< Runs a script when the player presses A facing the cell.
   NUM_EVENT_KINDS   ///< Number of kinds.
};

inline constexpr uint16_t kNoEventCondition = 0xFFFF;   ///< Event::condition: none.
inline constexpr uint8_t kAnyDirection = 4;   ///< Event::direction: any (field::kNoDirection).
inline constexpr uint8_t kEventDirectionMask = 7;   ///< Event::direction: the direction's bits (0-3, or kAnyDirection).

/// @name Warp trigger bits of Event::direction (I3), above the direction
/// A step warp has kAnyDirection and no bit; a press warp a direction. kWarpNeedsFacing: the walker must already face
/// the direction (a press while facing elsewhere turns it first, as Gen III arrow warps). kWarpFromAdjacent: a door in
/// a wall: it fires when the walker stands on the neighbouring cell on the side opposite the direction, faces the
/// direction and presses it (the walker does not step; the game opens the door); implies kWarpNeedsFacing.
///@{
inline constexpr uint8_t kWarpNeedsFacing = 0x10;    ///< The walker must face the warp's direction.
inline constexpr uint8_t kWarpFromAdjacent = 0x20;   ///< Fires from the neighbouring cell (a door).
///@}

/// C10: Event::direction bit of a trigger that runs immediately: its script runs to its end at once when the walker
/// arrives (script::run_now(): no lock, no request; it must not wait), the walker keeps moving and the cell's other
/// events are still checked (a reference game's run-immediately coordinate events). A plain trigger has no bit.
inline constexpr uint8_t kTriggerImmediate = 0x40;

/// One map event (ROM, B-3), in its chunk's bucket.
struct Event
{
   uint8_t local;   ///< Cell within the chunk: x | y << 4 (0-7 each).
   EventKind kind;   ///< What it does.
   uint8_t elevation;   ///< Level it is on; 0 = any (events match by this rule, not World::elevations).
   uint8_t direction;   ///< Warp: the direction a press warp needs (kAnyDirection: a step warp);
      ///< Sign: the facing it needs (kAnyDirection: any); Trigger: kAnyDirection, C10: | kTriggerImmediate.
   uint16_t target;   ///< Warp: index into Map::warps; Trigger, Sign: script ID.
   uint16_t condition;   ///< Trigger: variable that must hold `value`; Sign: flag that hides it
      ///< while set; kNoEventCondition: none.
   int32_t value;   ///< Trigger: the variable's value that fires it.
};
CAPSULE_POD(Event, 12);

/// Warp::map of a warp whose destination the game decides when it fires (I3: Gen III's dynamic warps, e.g. the
/// exit of a shared interior back to where the player came from); Warp::x then holds the warp number the map data
/// gives, y is 0.
inline constexpr uint16_t kDynamicWarp = 0xFFFF;

/// Where a warp lands (ROM).
struct Warp
{
   uint16_t map;   ///< Index into World::maps, or kDynamicWarp.
   int16_t x;   ///< Landing cell column.
   int16_t y;   ///< Landing cell row.
   uint8_t transition;   ///< script::Transition, or script::kTransitionDefault (D-17).
   uint8_t reserved;   ///< 0.
};
CAPSULE_POD(Warp, 8);

/// How a map object moves on its own (B-21: Gen III's set; D-17: RPG Maker
/// XP's Approach and Custom).
enum class Movement : uint8_t
{
   Static,   ///< Stands facing its direction.
   LookAround,   ///< Stands, turning to a random direction now and then.
   Wander,   ///< Random steps within its range box around its cell.
   PaceVertical,   ///< Walks up and down within its range.
   PaceHorizontal,   ///< Walks left and right within its range.
   Schedule,   ///< Follows a clock schedule (C-05; not built: stands like Static).
   Approach,   ///< Steps towards the walker (D-17).
   Custom,   ///< Follows its page's move route (D-17; capsule/script/route.h).
   Turn,   ///< I3: stands, turning to a random choice of its table after a random delay.
   Roam,   ///< I3: after a random delay turns to a random choice of its table and steps there when it can.
   Rotate,   ///< I3: turns a quarter turn every kRotateFrames frames (kRotateClockwise argument).
   Patrol,   ///< I3: walks back and forth along its facing's line, turning at the ends of its range.
   Circuit,   ///< I3: walks a circuit of four directions (argument: 2 bits each), each leg to the end of its range.
   Mirror,   ///< I3: copies the walker's moves, turned (kMirror... argument).
   InPlace,   ///< I3: walks in place facing its direction (kInPlace... argument: the pace).
   Invisible,   ///< I3: stands, never drawn but solid.
   NUM_MOVEMENTS   ///< Number of movement types.
};

/// @name Movement arguments (I3; ObjectTemplate / ObjectPage movement_arg and movement_flags)
/// The movements I3 adds behave as the reference games' (Gen III) object movement types, frame for frame: each is a
/// small machine that faces, waits, chooses and steps in the same frames and draws random numbers (rng::gameplay)
/// at the same points (capsule/field/field.h). Range boxes: a range of 0 on an axis leaves that axis unbounded.
/// - Turn / Roam: movement_arg is a table of four choices (2 bits each, choice k = (arg >> 2k) & 3); a turn draws
///   one (or one of the first two with kMoveTwoChoices; the table may repeat a direction to weight it);
///   movement_flags hold kMoveTwoChoices, the delay table (kMoveDelaysShift) and kMoveWatchRunner (Turn).
/// - Rotate: movement_flags kRotateClockwise, kMoveWatchRunner.
/// - Circuit: movement_arg holds the four legs (2 bits each).
/// - Mirror: movement_arg holds its reference facing and the vehicle class whose terrain it keeps to.
/// - InPlace: movement_arg is the pace (kInPlace...).
/// - Static: movement_flags kStaticKeepsFacing.
///@{
inline constexpr uint8_t kMoveTwoChoices = 1;   ///< Turn / Roam: choose among the table's first two entries.
inline constexpr uint8_t kMoveDelaysShift = 4;   ///< Turn / Roam: shift of the delay table index.
inline constexpr uint8_t kMoveDelaysMask = 0x30;   ///< Turn / Roam: the delay table (kMoveDelays..., 0-1).
inline constexpr uint8_t kMoveDelaysMedium = 0;   ///< Delay table: 32, 64, 96 or 128 frames.
inline constexpr uint8_t kMoveDelaysShort = 1;   ///< Delay table: 32, 48, 64 or 80 frames.
/// Turn / Rotate: a spotter whose sight box holds a running walker turns to it even before its delay is over (the
/// directions of its table limit the turn; Rotate: any direction).
inline constexpr uint8_t kMoveWatchRunner = 0x40;
inline constexpr uint8_t kStaticKeepsFacing = 0x80;   ///< Static: keeps a facing a script gives it (no turn back).
inline constexpr uint8_t kRotateClockwise = 1;   ///< Rotate: clockwise (down, left, up, right); else counter-clockwise.
inline constexpr uint8_t kRotateFrames = 48;   ///< Rotate: frames between turns.
inline constexpr uint8_t kCircuitLegShift = 2;   ///< Circuit (and Turn / Roam choices): bits per entry.
inline constexpr uint8_t kMirrorFacingMask = 3;   ///< Mirror: its reference facing (the walker's moves are turned by
                                                  ///< the angle from the walker's facing at spawn to it).
inline constexpr uint8_t kMirrorClassShift = 2;   ///< Mirror: shift of the vehicle class whose terrain it keeps to.
inline constexpr uint8_t kMirrorClassMask = 0x1C;   ///< Mirror: that class + 1 (0: any cell it may enter).
inline constexpr uint8_t kInPlaceSlow = 0;   ///< InPlace: a walk in place every 32 frames (half-speed animation).
inline constexpr uint8_t kInPlaceWalk = 1;   ///< InPlace: every 16 frames (walking).
inline constexpr uint8_t kInPlaceJog = 2;   ///< InPlace: every 8 frames (the fast set of animations).
inline constexpr uint8_t kInPlaceRun = 3;   ///< InPlace: every 4 frames (the faster set).
///@}

/// What starts a map object's script (D-17, RPG Maker XP's triggers).
enum class Trigger : uint8_t
{
   Action,   ///< Pressing A facing it (talking).
   PlayerTouch,   ///< The walker walks into it (or onto it, when it is Through).
   EventTouch,   ///< It walks into the walker, or the walker into it.
   Autorun,   ///< Runs again and again while its page is active (the walker waits).
   Parallel,   ///< Runs alongside play while its page is active (a parallel context).
   NUM_TRIGGERS   ///< Number of triggers.
};

inline constexpr int kMaxObjects = 15;   ///< Objects shown at once (sprite slots 1-15; the walker has slot 0).
/// I3: object templates per map; more than kMaxObjects need the map's object_view (objects spawn near the walker).
inline constexpr int kMaxObjectTemplates = 64;
inline constexpr uint16_t kNoRoute = 0xFFFF;   ///< No move route.
inline constexpr uint16_t kNoSelfSwitches = 0xFFFF;   ///< ObjectTemplate::self_base: none.
inline constexpr uint8_t kNoLocation = 0xFF;   ///< Map::location: none.

/// A map object template (ROM, B-21, D-17): an NPC, spotter or pickup.
/// Its fields are the base page; ObjectPage entries replace some of them.
struct ObjectTemplate
{
   AssetKey sprite;   ///< "sprite:NAME" asset key (missing: the object is invisible but solid).
   int16_t x;   ///< Cell column.
   int16_t y;   ///< Cell row.
   uint16_t script;   ///< Script its trigger starts; kNoEventCondition: none.
   uint16_t flag;   ///< Flag that hides the object while set; kNoEventCondition: none.
   uint16_t until;   ///< Spotter: flag that turns its sight off while set; kNoEventCondition: none.
   uint16_t page_first;   ///< First entry of Map::object_pages.
   uint16_t route;   ///< Custom movement: index into Map::route_offsets; kNoRoute: none.
   uint16_t self_base;   ///< First bit of its self switches A-D in World::self_switches; kNoSelfSwitches.
   uint8_t elevation;   ///< Level (meets others by World::elevations; 0 = any by the default rule).
   uint8_t facing;   ///< field::Direction.
   Movement movement;   ///< How it moves.
   uint8_t range;   ///< Wander / pace range: x | y << 4 (cells each way, 0-7).
   uint8_t sight;   ///< Spotter sight in cells (0: not a spotter).
   uint8_t page_count;   ///< Pages (0-255).
   Trigger trigger;   ///< What starts its script.
   uint8_t motion;   ///< Speed (1-4) | frequency (1-5) << 4.
   uint8_t options;   ///< route::kOption... bits.
   uint8_t kind;   ///< F2: its kind (the world's object_kinds index + 1; field actions target it), or kNoObjectKind.
   uint8_t movement_arg;   ///< I3: the movement's argument (see "Movement arguments").
   uint8_t movement_flags;   ///< I3: the movement's flags (see "Movement arguments").
};
CAPSULE_POD(ObjectTemplate, 32);

/// ObjectTemplate::kind of an object without a kind (F2).
inline constexpr uint8_t kNoObjectKind = 0;

/// @name ObjectPage::fields bits: what the page replaces
///@{
inline constexpr uint16_t kPageSprite = 1;   ///< sprite
inline constexpr uint16_t kPageScript = 2;   ///< script
inline constexpr uint16_t kPageMovement = 4;   ///< movement, range and route
inline constexpr uint16_t kPageFacing = 8;   ///< facing
inline constexpr uint16_t kPageTrigger = 16;   ///< trigger
inline constexpr uint16_t kPageMotion = 32;   ///< speed and frequency
inline constexpr uint16_t kPageOptions = 64;   ///< options
inline constexpr uint16_t kPageHidden = 128;   ///< the object is absent while this page is the active one
///@}

/// @name ObjectPage::cond bits
///@{
inline constexpr uint8_t kPageVarEqual = 1;   ///< The variable must equal the value (default: at least).
///@}

/// An event page of a map object (ROM, D-17, RPG Maker XP's pages): while
/// all its conditions hold, the fields it names replace the base page's.
/// The object's last page whose conditions hold is the active one.
struct ObjectPage
{
   AssetKey sprite;   ///< kPageSprite: the sprite.
   int32_t value;   ///< Variable condition: the value (var >= value, or == with kPageVarEqual).
   uint16_t flag;   ///< Condition: this flag set; kNoEventCondition: none.
   uint16_t var;   ///< Condition: this variable; kNoEventCondition: none.
   uint16_t script;   ///< kPageScript: the script.
   uint16_t route;   ///< kPageMovement: the route; kNoRoute: none.
   uint16_t fields;   ///< kPage... bits.
   uint8_t self_mask;   ///< Condition: these self switches on (bit 0 = A).
   uint8_t cond;   ///< kPageVarEqual.
   Movement movement;   ///< kPageMovement: the movement type.
   uint8_t range;   ///< kPageMovement: the range.
   uint8_t facing;   ///< kPageFacing: the facing.
   Trigger trigger;   ///< kPageTrigger: the trigger.
   uint8_t motion;   ///< kPageMotion: speed | frequency << 4.
   uint8_t options;   ///< kPageOptions: the options.
   uint8_t movement_arg;   ///< I3: kPageMovement: the movement's argument.
   uint8_t movement_flags;   ///< I3: kPageMovement: the movement's flags.
};
CAPSULE_POD(ObjectPage, 28);

/// A step dispatcher subscriber (ROM, B-3): runs every period-th step.
struct StepHook
{
   uint16_t period_mask;   ///< Period - 1 (the period is a power of two).
   uint16_t phase;   ///< Step count (mod period) it runs on; the compiler spreads them.
};
CAPSULE_POD(StepHook, 4);

/// A tile animation (ROM, A-9).
///
/// DS1 (D-30 section 5): an animation may show its frames on several tile targets at different frame offsets, the
/// frames stored once. Its own first_tile / phase is its first target (offset 0); `targets` more follow in
/// Map::anim_targets, after those of the animations before it. Target k shows frame
/// ((clock + phase_k) / period + offset_k) mod frame_count, so a target's offset is any whole frame 0 to
/// frame_count - 1 and its phase a start delay within the period.
struct TileAnimation
{
   const uint32_t* frames;   ///< frame_count x tile_count 4bpp tiles.
   uint16_t first_tile;   ///< First map tile replaced.
   uint8_t tile_count;   ///< Tiles replaced.
   uint8_t frame_count;   ///< Frames.
   uint8_t period;   ///< Display frames per animation frame.
   uint8_t phase;   ///< Frames into the period at map entry (spreads uploads).
   uint8_t flags = 0;   ///< I3: kAnim... bits.
   uint8_t targets = 0;   ///< DS1: more targets in Map::anim_targets (0: first_tile only).
};
CAPSULE_POD(TileAnimation, sizeof(void*) + 8);   // 12 B on the GBA

/// DS1: one more tile target of a tile animation (ROM; Map::anim_targets): its frames replace tile_count tiles from
/// first_tile, frame `offset` ahead of the animation's own frame, starting `phase` frames into the period.
struct AnimTarget
{
   uint16_t first_tile;   ///< First map tile replaced.
   uint8_t offset;   ///< Frames ahead of the animation's frame, 0 to frame_count - 1.
   uint8_t phase;   ///< Frames into the period at map entry, 0 to period - 1.
};
CAPSULE_POD(AnimTarget, 4);

inline constexpr int kMaxAnimTargets = 64;   ///< DS1: more targets per map (Map::anim_targets entries).

/// @name TileAnimation::flags (I3)
/// An animation keeps the map's animation clock: at enter() its timer and frame are those it would have after
/// running since the map's entry, and a seamless shift (field::shift_map()) keeps the clock running.
///@{
inline constexpr uint8_t kAnimFromSheet = 1;   ///< At entry no frame is uploaded: the map's own tiles show until the
                                               ///< first frame change.
inline constexpr uint8_t kAnimReloadOnShift = 2;   ///< A seamless shift reloads its tiles: the map's own tiles show
                                                   ///< again until the next frame change.
inline constexpr uint8_t kAnimRestartOnShift = 4;   ///< A seamless shift restarts it (timer at its phase, frame 0)
                                                    ///< instead of keeping the clock.
///@}

/// I3: the box around the walker that map objects spawn in (Gen III's view): an object spawns when its template
/// cell enters the box and goes when both its cell and its template cell are outside it; all zero: every object
/// spawns at enter() (at most kMaxObjects templates). Cells left of / right of / above / below the walker's cell (the
/// cell a step goes to, from the step's first frame).
struct ObjectView
{
   int8_t left;    ///< Cells to the left.
   int8_t right;   ///< Cells to the right.
   int8_t up;      ///< Cells above.
   int8_t down;    ///< Cells below.
};
CAPSULE_POD(ObjectView, 4);

/// @name Door bindings (DS1, D-30 section 1)
/// A map's cells that animate as doors (asset kind Door, capsule/asset/asset.h): bound by metatile (a cell any of
/// whose tile layers shows the metatile, its dictionary id) or by cell (a warp's `"door"`). The binding's cell is
/// the door's anchor: its bottom-left cell.
///@{
inline constexpr int16_t kDoorAnyCell = -32768;   ///< DoorBinding::x of a metatile binding.

/// One door binding (ROM).
struct DoorBinding
{
   AssetKey door;   ///< "door:NAME" asset key.
   uint16_t metatile;   ///< Metatile binding: the dictionary id; cell binding: kNoMetatile.
   int16_t x;   ///< Cell binding: its column; metatile binding: kDoorAnyCell.
   int16_t y;   ///< Cell binding: its row.
   uint16_t reserved;   ///< 0.
};
CAPSULE_POD(DoorBinding, 12);
///@}

/// A compiled map (ROM).
struct Map
{
   const ChunkDirectoryEntry* chunks;   ///< Populated chunks.
   const uint16_t* grid;   ///< grid_w x grid_h chunk indices, 0xFFFF = none.
   const MapCell* cells;   ///< 64 attribute cells per chunk, rows of 8.
   const MetatileId* layer_cells;   ///< 64 metatile ids per chunk and tile layer, rows of 8.
   const Metatile* metatiles;   ///< By metatile id.
   const uint32_t* tiles;   ///< tile_count 4bpp tiles.
   const Color* palettes;   ///< palette_count x 15 colors (entries 1-15 of each bank).
   const MetatileId* border;   ///< Per tile layer, border_w x border_h metatile ids.
   const TileAnimation* animations;   ///< animation_count entries, or nullptr.
   const encounter::Ref* encounter_refs;   ///< encounter_ref_count tables by slot and method, or nullptr.
   TileLayer layers[kMaxLayers];   ///< Tile layers, layer_count used.
   int16_t grid_x0;   ///< Chunk column of grid column 0.
   int16_t grid_y0;   ///< Chunk row of grid row 0.
   uint16_t grid_w;   ///< Grid columns.
   uint16_t grid_h;   ///< Grid rows.
   uint16_t chunk_count;   ///< Populated chunks.
   uint16_t metatile_count;   ///< Metatiles.
   uint16_t tile_count;   ///< Tiles.
   Color backdrop;   ///< Color behind every layer.
   uint8_t palette_count;   ///< Palette banks used (0 to palette_count-1).
   uint8_t border_w;   ///< Border pattern width, metatiles.
   uint8_t border_h;   ///< Border pattern height, metatiles.
   uint8_t border_phase_x;   ///< Pattern offset, metatiles.
   uint8_t border_phase_y;   ///< Pattern offset, metatiles.
   uint8_t animation_count;   ///< Tile animations.
   uint8_t layer_count;   ///< Tile layers (0-kMaxLayers).
   uint16_t encounter_ref_count;   ///< Entries of encounter_refs.
   const Event* events;   ///< event_count events, bucketed by chunk, or nullptr.
   const Warp* warps;   ///< warp_count warp landings, or nullptr.
   uint16_t event_count;   ///< Events.
   uint8_t warp_count;   ///< Warps.
   uint16_t area;   ///< Area root (B-2): index of the outdoor map whose area this is.
   const ObjectTemplate* objects;   ///< object_count templates (local IDs 0 up), or nullptr.
   const ObjectPage* object_pages;   ///< Their pages, by object, or nullptr.
   const uint8_t* route_code;   ///< Move routes of its objects (capsule/script/route.h), or nullptr.
   const uint16_t* route_offsets;   ///< Start of each route in route_code, or nullptr.
   uint16_t object_page_count;   ///< Pages.
   uint16_t route_count;   ///< Routes.
   uint8_t object_count;   ///< Objects (0-kMaxObjects).
   uint8_t location;   ///< Location (D-17, overworld::kLocations), or kNoLocation.
   uint8_t flags = 0;   ///< kMapNoRun, kMapEscapePoint (generated maps with neither leave it out).
   uint8_t darkness = 0;   ///< F2: darkness level on entering, 0 (lit) to kMaxDarkness (left out when 0).
   uint16_t tags = 0;   ///< F2: map tags (bit n = the world's map_tags[n]; field actions' menu targets).
   ObjectView object_view{};   ///< I3: where its objects spawn (all zero: all at enter()).
   const AnimTarget* anim_targets = nullptr;   ///< DS1: its animations' more targets, or nullptr.
   const DoorBinding* doors = nullptr;   ///< DS1: its door bindings (cell bindings first), or nullptr.
   uint8_t door_count = 0;   ///< DS1: door bindings.
};

/// Whether a map spawns its objects by the walker's view (I3).
/// @param m  the map
/// @return true when its object_view is set
constexpr bool spawns_by_view(const Map& m)
{
   return m.object_view.left != 0 || m.object_view.right != 0 || m.object_view.up != 0 || m.object_view.down != 0;
}

/// @name Darkness and map tags (F2, D-27 section 3)
/// A dark map shows a lit box around the walker; the rest of the map layers and the sprites are darkened (the
/// overworld runner sets window 0 and the Darken blend each frame: register writes only, the same on every
/// target, since the soft video model has windows and blends too). Text and HUD layers (BG0 under the map plan)
/// are left alone. A script's `darkness N;` changes the level until the next map. C8: what each level looks like
/// is the world's data (World::darkness_levels: tools/map_compiler.py darkness_levels()), not the engine's.
///@{
inline constexpr uint8_t kMaxDarkness = 15;  ///< Most darkness levels a world defines (its darkest level's number).
inline constexpr int kMaxMapTags = 16;       ///< Map tags of a world (Map::tags bits).
///@}

/// One darkness level of the world (ROM; C8): the lit box around the walker and how dark the rest goes.
struct DarknessLevel
{
   uint8_t lit_half;     ///< Half size of the lit box, pixels (0: no box, all dark).
   uint8_t brightness;   ///< The Darken blend's coefficient outside it (0 none to 16 black).
};
CAPSULE_POD(DarknessLevel, 2);

/// A map's key and index (ROM), sorted by key.
struct MapKey
{
   AssetKey key;   ///< Asset Key v1 of "map:NAME".
   uint16_t index;   ///< Index into World::maps.
   uint16_t reserved;   ///< 0.
};
CAPSULE_POD(MapKey, 8);

/// Where play starts.
struct Start
{
   uint16_t map;   ///< Index into World::maps.
   int16_t x;   ///< Metatile column.
   int16_t y;   ///< Metatile row.
   uint8_t facing;   ///< field::Direction.
   uint8_t reserved;   ///< 0.
};
CAPSULE_POD(Start, 8);

/// @name Cell override sets (DS1, D-30 section 2)
/// Named scripted map changes: a set lists cells whose graphics change on one tile layer (another metatile of the
/// map's dictionary, or four map entries) while the set is applied (`override apply NAME;` / `override clear NAME;`
/// in scripts, set_override_set()). A set lasts until the map is left, or persists through a flag (applied while it
/// is set) or a variable (applied while it holds the set's value): the field applies such sets when it enters the
/// map. The sets of every map are one world list (a script names a set the world over); a set of another map than
/// the shown one changes nothing on screen. A door's cell override (set_cell_override()) is drawn over a set's cells.
///@{
inline constexpr int kMaxMapOverrideSets = 32;   ///< Override sets per map (a 32-bit mask of applied sets).
inline constexpr uint8_t kOverrideEntries = 1;   ///< OverrideCell::flags: value indexes World::override_entries.

/// How an override set persists (OverrideSet::persist).
enum class OverridePersist : uint8_t
{
   Map,   ///< Until the map is left.
   Flag,   ///< Applied while its flag (OverrideSet::id) is set.
   Var,   ///< Applied while its variable (OverrideSet::id) holds OverrideSet::value.
   NUM_OVERRIDE_PERSISTS   ///< Number of kinds.
};

/// One cell of an override set (ROM).
struct OverrideCell
{
   int16_t x;   ///< Metatile column.
   int16_t y;   ///< Metatile row.
   uint8_t layer;   ///< Tile layer (0 to the map's layer_count - 1).
   uint8_t flags;   ///< kOverrideEntries, or 0.
   uint16_t value;   ///< A metatile id of the map's dictionary (kNoMetatile: nothing), or with kOverrideEntries the
                     ///< index of its four entries in World::override_entries (entry index / 4).
};
CAPSULE_POD(OverrideCell, 8);

/// One override set (ROM), in World::override_sets: sorted by map.
struct OverrideSet
{
   uint16_t map;   ///< Index into World::maps.
   uint16_t first_cell;   ///< Its first cell in World::override_cells.
   uint16_t cell_count;   ///< Cells.
   uint16_t id;   ///< Flag or variable id (persist Flag / Var).
   int16_t value;   ///< persist Var: the value that applies it.
   OverridePersist persist;   ///< How it persists.
   uint8_t reserved;   ///< 0.
};
CAPSULE_POD(OverrideSet, 12);
///@}

/// @name Object draw order (DS1, D-30 section 4)
/// Which sprite covers which among the walker, the map objects and their effect sprites is a sub-priority each
/// sprite gets from its position (gfx::sprites::set_order(): lower is in front; OAM entries go out sorted by sprite
/// priority, then sub-priority, then the lower sprite in front). The rule is the world's data: with `b` the sprite's
/// bottom edge on screen (pixels, its jump lift not counted) and `e` its elevation,
///
///     sub = base[e] + (rows - (((b + bias) & mask) >> row_shift)) * row_step + add   (mod 256)
///
/// so lower on screen is in front and the elevation's base moves whole levels in front or behind. Without a world
/// rule kDefaultDrawOrder applies (rows of 16 pixels, no elevation bases).
///@{

/// The world's draw-order rule (ROM).
struct DrawOrder
{
   uint8_t base[kElevationLevels];   ///< Added for each elevation level.
   int8_t bias;   ///< Pixels added to the bottom edge first.
   uint8_t mask;   ///< Bits of (edge + bias) kept (0xFF: a wrapping 8-bit screen row).
   uint8_t row_shift;   ///< Shift from those pixels to a row.
   uint8_t rows;   ///< Rows counted from (the row is subtracted from it).
   uint8_t row_step;   ///< Sub-priority steps per row.
   uint8_t add;   ///< Added last.
   uint8_t reserved[kPadAfterTwoBytes];   ///< 0.
};
CAPSULE_POD(DrawOrder, 24);

/// The rule of a world without one: rows of 16 pixels, two steps each, lower on screen in front.
inline constexpr DrawOrder kDefaultDrawOrder{{}, 8, 0xFF, 4, 16, 2, 1, {0, 0}};

/// The sub-priority of a sprite by a rule (see "Object draw order").
/// @param rule       the rule
/// @param bottom     the sprite's bottom edge on screen, pixels (its lift left out)
/// @param elevation  its elevation level (higher bits are ignored)
/// @return 0 (front) to 255
constexpr uint8_t draw_order_of(const DrawOrder& rule, int32_t bottom, uint8_t elevation)
{
   const auto edge = static_cast<uint32_t>(bottom + rule.bias) & rule.mask;
   const auto row = static_cast<uint32_t>(edge >> rule.row_shift);
   return static_cast<uint8_t>(rule.base[elevation & kElevationMask] + (rule.rows - row) * rule.row_step + rule.add);
}
///@}

/// @name Cell interactions (C13b)
/// What pressing A facing a cell does when no map object and no sign on it answers (field::update()): the world's
/// ordered rules from the faced cell's behavior to a script (a PC, a bookshelf, a wall map ...). The first rule whose
/// conditions all hold starts its script (field::Source::Interaction, the faced cell as the event's cell); a rule
/// with kInteractAfterActions is tried only after the field actions' dispatcher (field::set_action_rules()) had
/// nothing for the cell, so a fallback ("you can't use that here") may follow a field action.
///@{
inline constexpr uint8_t kAnyVehicle = 0xFF;   ///< Interaction::vehicle: any vehicle class.
inline constexpr uint8_t kInteractSameLevel = 1;   ///< Interaction::flags: the faced level (the walker's, or the
                                                   ///< level of its cell when that level meets every level and does
                                                   ///< not keep the mover's) must be the faced cell's level.
inline constexpr uint8_t kInteractAfterActions = 2;   ///< Interaction::flags: tried after the field actions.

/// One rule (ROM), in World::interactions in the world's order.
struct Interaction
{
   uint16_t script;   ///< The ScriptId it starts.
   uint16_t flag;   ///< A flag that must be set, or kNoEventCondition.
   uint8_t behavior;   ///< The faced cell's behavior (index into World::behaviors).
   uint8_t facing;   ///< The walker's facing it needs (field::Direction), or kAnyDirection.
   uint8_t vehicle;   ///< The walker's vehicle class it needs, or kAnyVehicle.
   uint8_t flags;   ///< kInteractSameLevel, kInteractAfterActions.
};
CAPSULE_POD(Interaction, 8);
///@}

/// Every map and the shared terrain classes (ROM). Generated by
/// tools/map_compiler.py; the engine carries an empty default.
struct World
{
   const Map* maps;   ///< map_count maps.
   const MapKey* keys;   ///< map_count keys, sorted.
   const Behavior* behaviors;   ///< behavior_count terrain classes.
   const Elevation* elevations;   ///< The 16 levels (A1), or nullptr: the default rule.
   const char* const* behavior_names;   ///< Debug names, or nullptr (--debug-names).
   const char* const* map_names;   ///< Debug names, or nullptr.
   const encounter::Bank* encounters;   ///< Encounter tables (D-12), or nullptr.
   const StepHook* step_hooks;   ///< step_hook_count hooks by ID (B-3), or nullptr.
   uint8_t* self_switches;   ///< Self switches of map objects (RAM, permanent, D-17), or nullptr.
   uint16_t map_count;   ///< Maps.
   uint16_t behavior_count;   ///< Terrain classes.
   uint16_t self_switch_bytes;   ///< Bytes of self_switches (part of the save).
   uint8_t vehicle_class_count;   ///< Vehicle classes (class 0 = on foot).
   uint8_t step_hook_count;   ///< Step hooks.
   Start start;   ///< Starting position.
   const VehicleClass* vehicle_classes;   ///< vehicle_class_count movement modes (A2), or nullptr: all as
                                          ///< kDefaultVehicle.
   /// C8: the darkness levels (level n is entry n - 1), or nullptr: every map lit.
   const DarknessLevel* darkness_levels = nullptr;
   uint8_t darkness_level_count = 0;      ///< C8: the darkest level (0 to kMaxDarkness).
   const DrawOrder* draw_order = nullptr;   ///< DS1: the object draw-order rule, or nullptr (kDefaultDrawOrder).
   const OverrideSet* override_sets = nullptr;   ///< DS1: every map's override sets, sorted by map, or nullptr.
   const OverrideCell* override_cells = nullptr;   ///< DS1: their cells, set by set.
   const uint16_t* override_entries = nullptr;   ///< DS1: map entries of cells with kOverrideEntries, 4 each.
   uint16_t override_set_count = 0;   ///< DS1: override sets.
   const Interaction* interactions = nullptr;   ///< C13b: the cell interaction rules, or nullptr.
   uint16_t interaction_count = 0;   ///< C13b: rules.
};

/// The build's world, generated at build time.
extern const World kWorld;

/// Finds a map by key.
/// @param key  key_v1("map:NAME")
/// @return the map, or nullptr
const Map* find(AssetKey key);

/// @name Cell queries (any map; border-aware)
///@{

/// The attribute cell at a metatile position: the painted cell, or
/// kCellBorder (impassable, behavior 0) outside the chunks and on
/// unpainted cells. N2: on the shown map, an edited cell's edit and the shown terrain (set_cell(), set_layout()).
/// @param m  the map
/// @param x  metatile column
/// @param y  metatile row
/// @return the cell
MapCell cell_at(const Map& m, int x, int y);

/// The metatile a tile layer shows at a position (the border's where the
/// cell is unpainted).
/// @param m      the map
/// @param layer  tile layer, 0 to layer_count - 1
/// @param x      metatile column
/// @param y      metatile row
/// @return metatile id, or kNoMetatile
MetatileId metatile_at(const Map& m, int layer, int x, int y);

/// Whether a position holds a painted cell (not the border).
/// @param m  the map
/// @param x  metatile column
/// @param y  metatile row
/// @return true inside a chunk on a painted cell
bool is_painted(const Map& m, int x, int y);

/// A cell's terrain class.
/// @param c  the cell
/// @return the behavior (behavior 0 if the table is shorter)
const Behavior& behavior(MapCell c);

/// An event on a cell: the first one of @p kind on (x, y) whose elevation
/// matches (the event's is 0, or equal to @p elevation), after @p after.
/// Looks only at the cell's chunk. Several triggers or signs may share a
/// cell; callers walk them with @p after until one's condition holds.
/// @param m          the map
/// @param x          metatile column
/// @param y          metatile row
/// @param kind       which kind
/// @param elevation  the walker's level
/// @param after      a match returned before, to find the next; nullptr for the first
/// @return the event, or nullptr
const Event* find_event(const Map& m, int x, int y, EventKind kind, uint8_t elevation, const Event* after = nullptr);

/// A level's entry: World::elevations', or the default rule's.
/// @param level  0-15 (higher bits are ignored)
/// @return its entry
const Elevation& elevation(uint8_t level);

/// Whether two levels meet (either one's meet mask holds the other): map
/// objects and the walker block each other only when their levels meet.
/// @param a  a level
/// @param b  another
/// @return true when they do
bool levels_meet(uint8_t a, uint8_t b);

/// Whether a mover on a level may enter a cell on another: they meet, but a
/// mover on a "keep" level meets by the cell's mask alone.
/// @param mover  the mover's level
/// @param cell   the cell's level
/// @return true when it may
bool may_enter(uint8_t mover, uint8_t cell);

/// The level a mover has after arriving on a cell: the cell's, or its own
/// when the cell's level keeps it (bridges).
/// @param mover  the mover's level
/// @param cell   the cell's level
/// @return the new level
uint8_t arrival_level(uint8_t mover, uint8_t cell);

/// The level a mover placed on a cell takes: the cell's, or kElevationAny
/// when the cell's level keeps the mover's (it has none to keep).
/// @param cell  the cell's level
/// @return the level
uint8_t placed_level(uint8_t cell);

/// A movement mode's entry: World::vehicle_classes', or kDefaultVehicle.
/// @param index  vehicle class
/// @return its entry
const VehicleClass& vehicle_class(uint8_t index);

/// The index of a map in World::maps.
/// @param m  a map of kWorld
/// @return its index
uint16_t index_of(const Map& m);

/// The asset key of a map (C8: what a save keeps, e.g. the escape point; find() turns it back).
/// @param index  index into World::maps
/// @return its key, or AssetKey{} (0) for no such map
AssetKey key_of(uint16_t index);
///@}

/// @name The shown map
///@{

/// Shows a map: loads its tiles and palettes, configures the BGs of its
/// tile layers and draws the view around the camera. Other BGs are not
/// touched.
/// @param m                the map
/// @param camera_x         camera left edge, pixels (metatile 0 starts at 0)
/// @param camera_y         camera top edge, pixels
/// @param keep_animations  I2: an animation the map shown before also ran (the same frames, first tile, period and
///                         frame count) keeps its timer and frame instead of starting over (a seamless step onto a
///                         connected map, field::shift_map())
void enter(const Map& m, int32_t camera_x, int32_t camera_y, bool keep_animations = false);

/// Hides the shown map's tile layers and forgets the map.
void leave();

/// @name Suspending the shown map (UC, D-33: a full-screen scene over the field)
/// suspend() hands the map's video memory (its character blocks, screen blocks, layers and the map plan) to someone
/// else and keeps everything else: the map, its terrain, the camera, the edited cells, the cell overrides, the
/// applied override sets and the animations' state. While suspended, update() and redraw() do nothing (nothing else
/// may draw the map). resume() loads the tiles and palettes again, configures the layers under the map plan and
/// draws the whole view as it was, as the reference games redraw their map view on returning to the field
/// (InitMapView: the tilesets copied again, the view drawn from the map's cells as they are now). Tiles a game
/// loaded over the map's (load_tiles(): a door's frames) are not reloaded.
///@{

/// Suspends the shown map (nothing when none is shown or it is suspended already).
void suspend();

/// Resumes a suspended map (nothing otherwise).
/// @param restart_animations  the tile animations start over from frame 0 and timer 0 (the reference games restart
///                            theirs on returning to the field); false keeps their timers and frames
void resume(bool restart_animations);

/// Whether the shown map is suspended.
/// @return true between suspend() and resume()
bool suspended();
///@}

/// The shown map.
/// @return the map, or nullptr
const Map* current();

/// Moves the camera; newly visible metatiles are drawn.
/// @param x  left edge, pixels
/// @param y  top edge, pixels
void set_camera(int32_t x, int32_t y);

/// Camera left edge.
/// @return pixels
int32_t camera_x();

/// Camera top edge.
/// @return pixels
int32_t camera_y();

/// Advances tile animations one frame. Call once per frame while a map is shown.
void update();

/// Redraws every visible metatile (after cells or VRAM were changed by other code).
void redraw();

/// @name Cell overrides (I3: door animations)
/// A cell override shows other map entries on a cell (a door's frames) until it is cleared; overrides are kept
/// while the camera moves and dropped by enter() and leave(). load_tiles() writes tiles into the shown map's tile
/// area (they stay until the next enter(), as when a game copies door frames over part of its tileset).
///@{
inline constexpr int kMaxCellOverrides = 8;   ///< Cells overridden at once.

/// Shows given map entries on a cell, on every tile layer.
/// @param x        metatile column
/// @param y        metatile row
/// @param entries  layer_count x 4 map entries (layer by layer, each top-left, top-right, bottom-left,
///                 bottom-right; tile indices count from the map's tile 0, palette bank included); copied
/// @return false when kMaxCellOverrides cells are overridden already (nothing changes)
bool set_cell_override(int x, int y, const uint16_t* entries);

/// Shows a cell's own metatiles again.
/// @param x  metatile column
/// @param y  metatile row
void clear_cell_override(int x, int y);

/// Writes 4bpp tiles into the shown map's tile area (must-land: they show with the next frame).
/// @param first  first map tile written (0 to kMaxTiles - 1)
/// @param tiles  count x 8 words (stays valid until the upload lands: ROM data)
/// @param count  tiles
void load_tiles(uint16_t first, const uint32_t* tiles, uint16_t count);

/// DS1: the door bound to a cell of the shown map: its cell binding, else the first metatile binding whose metatile
/// one of the cell's tile layers shows.
/// @param x  metatile column
/// @param y  metatile row
/// @return the binding, or nullptr (no door there, no map shown)
const DoorBinding* door_at(int x, int y);
///@}

/// @name Override sets of the shown map (DS1; see "Cell override sets")
///@{

/// Applies or clears an override set; a set of another map than the shown one changes nothing (its persistence is
/// the caller's: the script port writes its flag or variable).
/// @param set  index into World::override_sets
/// @param on   apply it (else clear it)
/// @return false when the set is not one of the shown map's (nothing changes)
bool set_override_set(uint16_t set, bool on);

/// Whether an override set is applied on the shown map.
/// @param set  index into World::override_sets
/// @return true while it is
bool override_set_on(uint16_t set);
///@}

/// DS1: a sprite's sub-priority by the world's draw-order rule (World::draw_order, or kDefaultDrawOrder).
/// @param bottom     its bottom edge on screen, pixels (its jump lift left out)
/// @param elevation  its elevation level
/// @return 0 (front) to 255
uint8_t sub_priority(int32_t bottom, uint8_t elevation);
///@}

/// @name Cell edits and layouts of the shown map (N2, D-32)
/// A game changes the shown map's cells while it plays (a switch pressed, a door slid open, a floor cracked):
/// set_cell() gives a cell new attributes and new metatiles on its tile layers until another map is shown (enter(),
/// leave(), set_layout()); cell_at(), metatile_at(), is_painted() and the drawing read the edited cell (edits apply to
/// the shown map only, so a query of another map reads its own cells). An edit changes only the data: the screen
/// shows it once draw_cell() or redraw() draws the cell, or when the cell scrolls into view (the reference games'
/// way: their scripts redraw the view after changing cells, their field code the one cell it changed). Edited cells
/// are kept in a fixed table (kMaxCellEdits, EWRAM); set_cell() on an edited cell replaces its edit.
///
/// set_layout() shows the shown map with another map's terrain: its cells, tile layers, tiles, palettes, border,
/// tile animations and metatile door bindings, while the shown map keeps its own events, objects, warps, override
/// sets and everything else (a game's other layout of a room, chosen by its map scripts as the map loads). The
/// terrain stays until the next enter() or leave().
///@{
inline constexpr int kMaxCellEdits = 512;   ///< Cells edited at once (a field of ash grass the walker clears).

/// Gives a cell of the shown map new attributes and metatiles (data only; see draw_cell()).
/// @param x       metatile column
/// @param y       metatile row
/// @param cell    the cell's new attributes (behavior, collision, elevation, ...)
/// @param layers  per tile layer of the shown terrain its new metatile id (kNoMetatile: nothing), layer_count ids
/// @return false when no map is shown or kMaxCellEdits other cells are edited already (nothing changes)
bool set_cell(int x, int y, MapCell cell, const MetatileId* layers);

/// Draws one cell of the shown map again when it is in the drawn window (its edit, override set or door frame).
/// @param x  metatile column
/// @param y  metatile row
void draw_cell(int x, int y);

/// The shown map's edited cells.
/// @return 0 to kMaxCellEdits
uint16_t cell_edit_count();

/// C11: whether a cell of the shown map is edited (set_cell()): a game saves the edits in its view as the reference
/// games save their map view (SaveMapView) and gives them back with set_cell() when the saved game resumes.
/// @param x  metatile column
/// @param y  metatile row
/// @return true when it is (false without a shown map)
bool is_edited(int x, int y);

/// Shows the shown map with another map's terrain (see above), or its own again; loads the terrain's graphics and
/// draws the view (the cell edits are dropped). Nothing happens without a shown map.
/// @param layout  a map of kWorld whose terrain shows, or nullptr: the shown map's own
void set_layout(const Map* layout);

/// The map whose terrain shows (the shown map's own unless set_layout() chose another).
/// @return it, or nullptr without a shown map
const Map* terrain();
///@}

}   // namespace capsule::map
