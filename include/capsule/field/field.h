/// @file
/// Overworld traversal (B-3, E-7, first part): one walker moving on the shown
/// map's grid, with collision, elevation and data-driven terrain behaviors.
///
/// Movement is cell by cell (16 pixels) at the speeds of the walker's
/// vehicle class (A2, map::VehicleClass; on foot by default walking at 1
/// pixel per frame, running at 2, the E-7 speed tiers). Running needs a class
/// that may run on a map that allows it (can_run()); otherwise a run walks.
/// set_vehicle() changes the class (scripts: `vehicle NAME;`). Pressing a
/// new direction while standing turns in place first; holding it then
/// walks. A step is allowed when the target cell
///   - has collision 0,
///   - is on a level the walker's meets (A1, map::may_enter(): the world's
///     elevation table; by default 0 meets any level, 15 (multi-level) any,
///     1-14 themselves),
///   - has a behavior passable by the walker's vehicle class (0 = on foot).
///
/// Arriving on a cell gives the walker the cell's level (by default 0
/// included: a level-0 cell such as stairs lets it step off onto any level),
/// unless the cell's level keeps the mover's (by default 15, a bridge: it
/// stays on top of or under the bridge); see map::arrival_level().
///
/// Behaviors: a Ledge cell is jumped over when moving in its direction
/// (landing on the cell beyond, which must itself be enterable; N2: whatever
/// the ledge cell's own collision, as the reference games test the ledge
/// first) and blocks otherwise; arriving on a Slide cell keeps the walker moving the same way
/// until blocked; arriving on a Push cell forces a step in its direction.
///
/// The walker's sprite (a sprite slot with an asset following the animation
/// convention below) is drawn with its bottom edge on the cell's, at the
/// sprite priority World::elevations gives for its elevation, and the
/// camera follows it (centred).
///
/// Encounters (D-12): arriving on an encounter cell rolls the world's
/// step encounter methods for the walker's vehicle class (encounter::
/// roll_step(), on rng::gameplay). When one starts, the walker stops on that
/// cell (no slide or push) and takes no new step until the game collects it
/// with take_encounter(). Noclip never rolls; set_encounters(false) turns
/// rolls off (cutscenes, or game code that keeps encounters away for a while).
///
/// Map events (B-3): arriving on a cell checks, in the reference games' order, its
/// triggers (scripts; one with a variable condition only while the variable
/// holds its value; the first that holds fires), then a step warp, then the step hooks that are due, then the
/// encounter roll; the first that fires stops the walker there (no
/// slide or push) and becomes the pending request. Pressing A (Intent::
/// interact) while standing checks, in the reference games' order, a map object
/// on the faced cell (across a counter: the cell beyond), a sign on it (its facing
/// rule, and its flag clear), the world's cell interactions of its behavior
/// (C13b, map::Interaction), the field actions (F2), then the interactions kept for
/// after them; pressing the direction of a press warp while
/// standing on it warps. Events match the walker's elevation (an event on
/// level 0 matches any; C13b: facing a cell, the level is that of the walker's
/// cell when that level meets every level and does not keep the mover's, as the
/// reference games' faced position takes a stairs cell's level). The game collects a request with take_event(): it
/// runs the script (capsule/script/script.h) or calls warp(), and the walker
/// takes no step until then.
///
/// Step hooks: every period-th step (World::step_hooks, power-of-two periods,
/// phases spread by the compiler) game::step_hook() runs with the hook's ID
/// and may ask for a script; only the first script asked for in a step runs.
/// A step on which a trigger or warp fires runs no hooks (as the reference games' step
/// scripts).
///
/// Changing maps: enter() shows another map with the walker on a cell, keeps
/// its step counters, clears the map (and, across areas, the area) flags and
/// variables (B-2, script::change_map()) and runs the map's load scripts
/// (script::enter_map()).
///
/// Map objects (B-21): see "Map objects" below; they spawn in enter(), block
/// the walker, talk on A (before signs) and, as spotters, spot the walker
/// (before the arrival events). capsule::script::world (the object
/// commands of scripts) is implemented here.
///
/// I3 (D-29): a map with an object view (map::ObjectView) spawns its objects
/// (up to map::kMaxObjectTemplates) as the walker comes near and drops them
/// as it leaves, sprite slots taken from a pool; the movements I3 adds
/// (map::Movement Turn to Invisible) run the reference games' machines
/// frame for frame (map.h "Movement arguments"); walls stop objects too;
/// arrow warps need the walker's facing and door warps fire from the cell
/// in front (map::kWarpFromAdjacent); A on a Counter behavior talks to the
/// object beyond it; a vehicle class may set its turn time and bump or keep
/// moving (map::kVehicleBumps, kVehicleKeepsMoving).
///
/// DS1 (D-30): every object's sprite, the walker's included, takes its
/// sub-priority each frame from the world's draw order (map::sub_priority():
/// its bottom edge on screen and its elevation) and the sprites are drawn
/// sorted (gfx::sprites::set_sorted()); update() ends by advancing the doors
/// (capsule/field/doors.h), and enter() / shift_map() stop them and apply the
/// map's persisted override sets.
///
/// Sprite animation convention (the walker and objects): 0-3 standing facing
/// down, up, left, right; 4-7 walking; 8-11 running (optional: walking is
/// used without them); I3: 12-15 the fast set (turning in place, jogging in
/// place), 16-19 the faster set (running in place). A sprite with a step
/// table (asset::Sprite::steps) shows its walk animation paused on a
/// standing entry while it stands, and its steps alternate the feet. The walker's vehicle class shifts its set: class
/// animations start at map::VehicleClass::first_animation (missing ones fall
/// back to the class's standing ones, then to 0-3).
#pragma once

#include "capsule/asset/asset.h"
#include "capsule/encounter/encounter.h"
#include "capsule/gfx/slots.h"
#include "capsule/map/map.h"
#include "capsule/script/data.h"
#include "capsule/script/route.h"

#include <cstdint>

/// Overworld traversal.
namespace capsule::field
{

/// A grid direction.
enum Direction : uint8_t
{
   DIR_DOWN,         ///< +y.
   DIR_UP,           ///< -y.
   DIR_LEFT,         ///< -x.
   DIR_RIGHT,        ///< +x.
   NUM_DIRECTIONS    ///< Number of directions; also "none".
};

inline constexpr uint8_t kNoDirection = NUM_DIRECTIONS;   ///< Intent::direction when no direction is held.
inline constexpr uint8_t kWalkSpeed = map::kDefaultWalkSpeed;   ///< Pixels per frame of scripted walks (on foot).
inline constexpr uint8_t kRunSpeed = map::kDefaultRunSpeed;   ///< Pixels per frame sliding, jumping, pushed.
inline constexpr uint8_t kTurnFrames = 6;                 ///< Frames a turn in place takes.
inline constexpr uint8_t kJumpHeight = 8;                 ///< Peak height of a ledge jump, pixels.

/// Column change of a direction.
/// @param d  the direction
/// @return -1, 0 or 1
constexpr int dx_of(uint8_t d)
{
   return d == DIR_LEFT ? -1 : (d == DIR_RIGHT ? 1 : 0);
}

/// Row change of a direction.
/// @param d  the direction
/// @return -1, 0 or 1
constexpr int dy_of(uint8_t d)
{
   return d == DIR_UP ? -1 : (d == DIR_DOWN ? 1 : 0);
}

/// What the player (or a script, or a test) asks for this frame.
struct Intent
{
   uint8_t direction = kNoDirection;   ///< Direction held, or kNoDirection.
   bool run = false;                   ///< Run instead of walk (walks unless can_run()).
   bool interact = false;              ///< A was pressed this frame (signs).
};

inline constexpr uint16_t kNoScript = 0xFFFF;   ///< game::step_hook(): no script.

/// What a request asks the game to do.
enum class Request : uint8_t
{
   None,           ///< Nothing pending.
   Script,         ///< Run FieldEvent::script.
   Warp,           ///< Call warp() (another map, or another cell of this one).
   NUM_REQUESTS    ///< Number of requests.
};

/// Where a request came from.
enum class Source : uint8_t
{
   Warp,           ///< A warp event.
   Trigger,        ///< A trigger event.
   Sign,           ///< A sign event.
   StepHook,       ///< A step hook.
   Object,         ///< Talking to a map object (B-21).
   Spotter,        ///< A spotter saw the walker and walked up (B-21).
   Touch,          ///< A Player Touch or Event Touch object (D-17).
   Autorun,        ///< An Autorun object (D-17).
   Action,         ///< A field action on a faced behavior or object (F2, D-27 section 3).
   Interaction,    ///< C13b: a cell interaction rule of the faced cell's behavior (map::Interaction).
   NUM_SOURCES     ///< Number of sources.
};

inline constexpr uint8_t kNoObject =
   script::kNoSelf; ///< FieldEvent::object: none (pass it to script::start() as self).

/// A request from the field to the game (B-3).
struct FieldEvent
{
   Request what;      ///< What to do.
   Source source;     ///< Why.
   uint16_t script;   ///< Script: its ID.
   uint16_t map;      ///< Warp: destination map index.
   int16_t x;         ///< Warp: landing column.
   int16_t y;         ///< Warp: landing row.
   int16_t at_x;      ///< Cell of the event (sign: the faced cell).
   int16_t at_y;      ///< Its row.
   uint8_t object = kNoObject;   ///< Object, Spotter, Touch, Autorun: the object's local ID; else kNoObject.
   uint8_t transition = script::kTransitionDefault;    ///< Warp: script::Transition, or script::kTransitionDefault.
};

inline constexpr uint8_t kFirstObjectSlot = gfx::slots::kFirstObject;   ///< Sprite slot of object 0 (object n: 1 + n).
inline constexpr uint8_t kWalkerSlot = gfx::slots::kWalker;              ///< The walker's sprite slot.
static_assert(map::kMaxObjects <= gfx::slots::kObjects, "capsule/gfx/slots.h: one sprite slot per map object");
inline constexpr uint8_t kSpotFrames = 30;          ///< Frames a spotter waits after seeing the walker.

/// What the walker is doing.
enum class Motion : uint8_t
{
   Idle,          ///< Standing.
   Turning,       ///< Turning in place.
   Walking,       ///< Taking a step it chose.
   Jumping,       ///< Jumping a ledge (two cells).
   Sliding,       ///< Carried by a Slide cell.
   Pushed,        ///< Carried by a Push cell.
   NUM_MOTIONS    ///< Number of motions.
};

/// Why the last attempted step did not happen.
enum class Blocked : uint8_t
{
   None,          ///< It happened (or none was attempted).
   Collision,     ///< Target cell has collision.
   Elevation,     ///< Target cell is on another level.
   Terrain,       ///< Target behavior is not passable for the vehicle class.
   Ledge,         ///< A ledge faces another way (or its landing is blocked).
   Object,        ///< A map object stands there (B-21).
   Rule,          ///< N2: the game's step rule refused it (set_step_rule(); the walker stops without a bump).
   NUM_BLOCKED    ///< Number of reasons.
};

/// The walker (RAM).
struct Walker
{
   int16_t x;                  ///< Cell column (the destination while moving).
   int16_t y;                  ///< Cell row.
   int32_t px;                 ///< Left edge of its 16x16 cell box, pixels.
   int32_t py;                 ///< Top edge, pixels.
   uint32_t steps;             ///< Steps completed.
   uint32_t encounter_steps;   ///< Steps that ended on encounter cells.
   uint8_t facing;             ///< Direction.
   uint8_t elevation;          ///< Current level, 0-15.
   Motion motion;              ///< What it is doing.
   uint8_t move_dir;           ///< Direction of the current step.
   uint8_t speed;              ///< Pixels per frame of the current step.
   uint8_t remaining;          ///< Pixels left in the current step.
   uint8_t timer;              ///< Frames left of a turn.
   uint8_t lift;               ///< Height above the ground while jumping, pixels.
   uint8_t vehicle_class;      ///< Movement mode (map::VehicleClass), 0 = on foot; set_vehicle().
   bool noclip;                ///< Ignore collision, elevation and terrain (debug).
   bool visible;               ///< Its sprite shows (routes can hide it).
   uint8_t options;            ///< route::kOption... bits (Through, direction fix, animations, on top).
   bool running;               ///< The current step is a run.
   Blocked last_blocked;       ///< Result of the last attempted step.
};

inline constexpr uint8_t kHiddenPage =
   0xFF;   ///< Object::page: the active page hides it (objects have at most 254 pages).

/// A map object (RAM, B-21, D-17), spawned from its map::ObjectTemplate and
/// its active event page.
struct Object
{
   const asset::Sprite* sprite;   ///< Its sprite asset, or nullptr (invisible, still solid).
   int32_t px;                    ///< Left edge of its 16x16 cell box, pixels.
   int32_t py;                    ///< Top edge, pixels.
   AssetKey sprite_key;           ///< Key of its sprite (the active page's).
   int16_t x;                     ///< Cell column (the destination while moving).
   int16_t y;                     ///< Cell row.
   int16_t from_x;                ///< Cell it is leaving while moving (x, y when standing).
   int16_t from_y;                ///< Its row.
   int16_t home_x;                ///< C12: the cell it spawned on (its template cell then, a script's move
                                  ///< included): the centre of its range and what keeps it spawned by the view.
   int16_t home_y;                ///< C12: its row.
   uint16_t script;               ///< Script of the active page; kNoScript: none.
   uint16_t timer;                ///< Frames to its next movement decision.
   uint16_t steps_left;           ///< Scripted steps still to take after this one.
   uint16_t route;                ///< Custom movement route of the active page (map::kNoRoute: none).
   uint8_t facing;                ///< Direction.
   uint8_t base_facing;           ///< Facing of the active page (Static objects return to it).
   uint8_t elevation;             ///< Level; 0 = any.
   map::Movement movement;        ///< Movement of the active page.
   map::Trigger trigger;          ///< Trigger of the active page.
   uint8_t range;                 ///< Range of the active page: x | y << 4.
   uint8_t move_dir;              ///< Direction of the current step.
   uint8_t next_dir;              ///< Direction of the scripted steps left.
   uint8_t remaining;             ///< Pixels left in the current step (or frames of a jump).
   uint8_t anim;                  ///< Animation playing, or 0xFF.
   uint8_t speed;                 ///< 1-4 (route::kDefaultSpeed = walking).
   uint8_t frequency;             ///< 1-5.
   uint8_t options;               ///< route::kOption... bits.
   uint8_t page;                  ///< Active page: 0 = its own fields, n = page n, kHiddenPage.
   uint8_t lift;                  ///< Height above the ground while jumping, pixels.
   bool present;                  ///< Spawned (not hidden by its flag or a script).
   bool active;                   ///< Present and its active page shows it.
   bool moving;                   ///< Taking a step or a jump.
   uint8_t jump_frames;           ///< Frames of the jump under way (move_dir NUM_DIRECTIONS).
   bool visible;                  ///< Its sprite shows (routes can hide it; it still blocks).
   uint8_t slot;                  ///< I3: its sprite slot (gfx/slots.h), or 0xFF while it has none.
   uint8_t movement_arg;          ///< I3: the active page's movement argument (map.h "Movement arguments").
   uint8_t movement_flags;        ///< I3: the active page's movement flags.
   uint8_t machine;               ///< I3: the step of its movement's machine (the movements I3 adds).
   uint8_t sequence;              ///< I3: Patrol / Circuit: the leg; Mirror: the walker's facing + 1 at spawn.
   uint16_t delay;                ///< I3: frames its machine still waits.
   uint8_t in_place;              ///< I3: frames left of a walk in place (0: none).
   uint8_t in_place_pace;         ///< I3: that walk's pace (map::kInPlace...).
   bool invisible;                ///< I3: never drawn (Invisible), still solid.
   bool blind;                    ///< C10: does not spot the walker until it spawns again (stop_sight()).
   bool fixed_order;              ///< N2: its sprite keeps `order` among the sprites (set_fixed_order()).
   uint8_t order;                 ///< N2: that sub-priority.
   int16_t sprite_dx;             ///< N2: its sprite drawn this many pixels right of its cell (set_sprite_offset()).
   int16_t sprite_dy;             ///< N2: and this many pixels down.
};

/// What the walker faces when A is pressed and nothing else answers (F2, D-27 section 3): a map object of a kind
/// without a script of its own, or a cell whose behavior it cannot enter as it is (water on foot).
struct ActionFace
{
   script::ActionTarget target;   ///< Behavior or Object.
   uint8_t value;                 ///< The world's behavior index, or the object's kind (map::ObjectTemplate::kind).
   uint8_t object;                ///< The object's local ID, or kNoObject.
   uint8_t facing;                ///< The walker's facing.
   int16_t x;                     ///< The faced cell's column.
   int16_t y;                     ///< Its row.
};

/// The field actions' dispatcher (capsule/battle/field_actions.h ActionScope attaches one): the script to run for
/// a faced target (the dispatcher checks the requirements against the party and writes the member), or kNoScript.
/// @param face  what the walker faces
/// @return a ScriptId, or kNoScript (no action is available)
using ActionRules = uint16_t (*)(const ActionFace& face);

/// Attaches the field actions' dispatcher (nullptr: A on such targets does nothing, as before F2).
/// @param rules  the dispatcher
void set_action_rules(ActionRules rules);

/// The attached dispatcher.
/// @return it, or nullptr
ActionRules action_rules();

/// Puts the walker on a cell of the shown map (map::enter() first), standing,
/// and centres the camera on it.
/// @param x       cell column
/// @param y       cell row
/// @param facing  Direction
void place(int x, int y, uint8_t facing);

/// @name Map objects (B-21)
/// The shown map's objects spawn in enter() (or spawn_objects()): those
/// whose hide flag is clear, with their first override whose flag is set
/// applied, each in sprite slot kFirstObjectSlot + its local ID. Objects
/// block the walker and each other (both the cell they stand on and, while
/// moving, the one they leave).
///
/// Movement: Static stands; LookAround turns at random every 32-128 frames;
/// Wander steps at random inside its range box around its cell; the pace
/// types walk to and fro within their range. Random choices come from
/// rng::gameplay (positions change what the walker can do).
///
/// Static objects keep any facing a script gives them until release().
///
/// Event pages (D-17): an object's last page whose conditions hold (flags,
/// a variable, self switches) sets its sprite, movement, route, speed,
/// frequency, options, script and trigger, or hides it. Pages are chosen
/// again whenever a flag, variable or self switch changes
/// (script::state_version()); a new page restarts its movement and facing.
///
/// Triggers (D-17): Action (A facing it); Player Touch (the walker walks
/// into it, or onto it when it is Through; A works too); Event Touch (it
/// walks into the walker, or the walker into it; A too); Autorun (requested
/// again and again while its page is active and nothing else runs);
/// Parallel (the game runs parallel_script() alongside play).
///
/// Movement options: walk / step animation, direction fix, Through (no
/// collision, blocks nothing), on top (sprite priority 0). Speeds 1-4 are
/// 1 pixel every 2 frames and 1, 2 and 4 pixels a frame; frequencies 1-5
/// scale how often its own movement acts (x4, x2, x1, x1/2, at once).
///
/// Talking: pressing A facing a standing object with a script turns it to
/// the walker and requests the script (Source::Object). Objects are checked
/// before signs.
///
/// Spotters (E-8): while the walker stands, a spotter whose sight is on
/// (its until flag clear) and which faces the walker within its sight,
/// with every cell in between passable and free, spots it: it waits
/// kSpotFrames, walks up to the cell next to the walker, the walker turns to
/// it, and its script is requested (Source::Spotter). The first spotter in
/// local-ID order wins; how many may approach together comes from the battle
/// formats, later.
///
/// Locking: from the moment a script is requested (any source) objects stop
/// moving on their own until release(), which the game calls when the script
/// ends; scripts the game starts itself (on_frame triggers) call lock().
/// Scripted moves (move_object()) still run. enter() releases too.
///
/// show_object() spawns on the template cell without checking it is free.
///@{

/// Spawns the shown map's objects (enter() does this, after the map's load
/// scripts, so on_transition / on_load change objects through their hide
/// and override flags, as in the reference games; object commands in load scripts find no
/// objects; I4: template changes they make hold). C10: before spawning,
/// enter() and shift_map() run the map's on_resume scripts, after it enter()
/// runs its on_warp_into scripts (script::run_map_scripts()). After map::leave() it hides every object sprite.
void spawn_objects();

/// Objects of the shown map.
/// @return map::Map::object_count, or 0 without a map
uint8_t object_count();

/// An object of the shown map.
/// @param local  local ID
/// @return its state, or nullptr when out of range
const Object* object(uint8_t local);

/// Shows (spawns at its template cell, overrides applied) or hides an object.
/// @param local  local ID
/// @param on     show it
void show_object(uint8_t local, bool on);

/// C10: stops an object spotting the walker until it spawns again (the next map, or a respawn): for a game that
/// stands in for what a spotter starts (a battle it does not run yet) without writing saved state. Never saved.
/// @param local  local ID
void stop_sight(uint8_t local);

/// C10: takes an object off the map until it may come back (a reference game's object removal): unlike
/// show_object(false) it is not held hidden; it spawns again at its template cell when the walker's next step looks
/// for objects (the view on a map with one, the whole map otherwise) and its hide flag is clear (an object
/// without a hide flag on the next look). A caller that keeps it away sets its hide flag first.
/// @param local  local ID
void remove_object(uint8_t local);

/// Turns an object.
/// @param local  local ID
/// @param dir    Direction
void face_object(uint8_t local, uint8_t dir);

/// Starts a move route (capsule/script/route.h) on an object, replacing its
/// own movement until it ends (a repeating route never does).
/// @param local        local ID, or (N2) kCameraObject: the camera mover's route
/// @param code         the route (stays valid)
/// @param from_script  started by a script (routes_busy() counts it unless it repeats)
/// @return false when the object is not active (N2: or the camera mover is not spawned)
bool route_object(uint8_t local, const uint8_t* code, bool from_script);

/// Starts a move route on the walker (respects collision unless Through).
/// @param code  the route (stays valid)
void route_walker(const uint8_t* code);

/// Whether a non-repeating route started by a script still runs.
/// @return true while one does
bool routes_busy();

/// A self switch of an object (D-17).
/// @param local   local ID
/// @param letter  0-3 (A-D)
/// @return its state; false for objects without self switches
bool self_switch(uint8_t local, uint8_t letter);

/// Sets a self switch (asserts for objects without self switches).
/// @param local   local ID
/// @param letter  0-3
/// @param on      set
void set_self_switch(uint8_t local, uint8_t letter, bool on);

/// The script a Parallel object runs alongside play (D-17).
/// @param local  local ID
/// @return its script, or kNoScript when its active page is not Parallel
uint16_t parallel_script(uint8_t local);

/// Puts an object on a cell at once (its route and steps stop).
/// @param local   local ID
/// @param x       column
/// @param y       row
/// @param facing  Direction, or NUM_DIRECTIONS to keep it
void place_object(uint8_t local, int x, int y, uint8_t facing);

/// Offsets the camera from the walker (camera pans and shakes), pixels.
/// @param dx  right
/// @param dy  down
void set_camera_offset(int32_t dx, int32_t dy);

/// Starts a scripted walk: @p steps cells in a direction, at walking speed,
/// through anything (as the reference games' scripted walks). Replaces a scripted walk in
/// progress.
/// @param local  local ID
/// @param dir    Direction
/// @param steps  cells (0: just turns)
/// @return false when the object is not active
bool move_object(uint8_t local, uint8_t dir, uint16_t steps);

/// Whether an object is still taking steps.
/// @param local  local ID, or (N2) kCameraObject
/// @return true while moving
bool object_moving(uint8_t local);

/// Turns the walker.
/// @param dir  Direction
void face_walker(uint8_t dir);

/// Starts a scripted walk of the walker: @p steps cells, at walking speed,
/// through anything, with no events, encounters or step counts.
/// @param dir    Direction
/// @param steps  cells (0: just turns)
void move_walker(uint8_t dir, uint16_t steps);

/// Whether a scripted walk of the walker would take every step by its movement rules (C8: `move player ... checked`;
/// nothing moves): from its cell, each step by check_step() with its elevation and vehicle class (a class that
/// dismounts steps onto land as class 0 and walks on as it), no ledge jump, no object in the way. A walker that
/// walks through anything (noclip, the Through option) always may.
/// @param dir    Direction
/// @param steps  cells
/// @return true when every step is allowed
bool walker_path_clear(uint8_t dir, uint16_t steps);

/// Whether a scripted walk of an object would take every step by an object's movement rules (C8: `move ... checked`;
/// may_enter() for each cell: passable, no ledge, no walker or other object; Through objects always may).
/// @param local  local ID
/// @param dir    Direction
/// @param steps  cells
/// @return false when a step is blocked or the object is not active
bool object_path_clear(uint8_t local, uint8_t dir, uint16_t steps);

/// Whether the walker is still taking scripted steps.
/// @return true while it is
bool walker_moving();

/// Locks objects (a script the field did not request starts, e.g. an
/// on_frame map script).
void lock();

/// Lets objects move on their own again; call when a field script ends.
/// Does nothing while another request is pending or a spotter approaches.
/// Re-applies every object's overrides (the script may have set their
/// flags) and turns Static objects back to their facing.
void release();

/// Whether objects are locked (a script was requested and not released).
/// @return true while locked
bool locked();

/// I4: shows or hides an object's sprite where it stands (a script's show / hide at a cell); it still blocks
/// and moves. The next page change or spawn shows it again.
/// @param local  local ID
/// @param on     shown
void set_object_visible(uint8_t local, bool on);

/// I4: moves an object's template cell on the shown map (a permanent position change): it spawns there from now on,
/// until the next map (enter() and shift_map() forget the change before the map's load scripts run).
/// @param local  local ID
/// @param x      column
/// @param y      row
void set_template_cell(uint8_t local, int x, int y);

/// I4: changes an object's template movement on the shown map (a movement type change), for every page,
/// from its next spawn or page change on, until the next map.
/// @param local     local ID
/// @param movement  the movement
/// @param arg       its movement_arg (map.h "Movement arguments")
/// @param flags     its movement_flags
/// @param facing    its facing, or NUM_DIRECTIONS to keep the template's
void set_template_movement(uint8_t local, map::Movement movement, uint8_t arg, uint8_t flags, uint8_t facing);

/// UC: an object's template changes on the shown map (set_template_cell(), set_template_movement()), as a save keeps
/// them (the reference games save their copy of the map's object templates).
struct TemplateChange
{
   bool cell;               ///< x and y replace the template's cell.
   bool movement;           ///< The movement fields replace the template's.
   int16_t x;               ///< The cell's column.
   int16_t y;               ///< Its row.
   map::Movement kind;      ///< The movement.
   uint8_t arg;             ///< Its movement_arg.
   uint8_t flags;           ///< Its movement_flags.
   uint8_t facing;          ///< Its facing, or NUM_DIRECTIONS (the template's).
};

/// UC: an object's template changes.
/// @param local  local ID
/// @return them (none for an ID past the map's)
TemplateChange template_change(uint8_t local);

/// UC: gives an object its template changes back (a saved game resuming; before its objects spawn).
/// @param local   local ID
/// @param change  template_change()'s
void set_template_change(uint8_t local, const TemplateChange& change);

/// UC: sets the cell an object counts as its spawn cell (Object::home_x / home_y: the centre of its range and what
/// keeps it spawned; the reference games save it as an object's initial coordinates).
/// @param local  local ID
/// @param x      column
/// @param y      row
void set_home(uint8_t local, int x, int y);
///@}

/// Loads the walker's sprite into its slot again (after a sequence used the
/// sprite slots, e.g. a location preview).
void reload_walker_sprite();

/// I3: shows or hides the walker's sprite (a door it walked into; it still blocks and takes input).
/// @param on  show it
void show_walker(bool on);

/// N2: a game's rule for the walker's steps (rotating gates: a step into a gate's arm turns the gate or is refused),
/// asked after the cell, its level and the objects let a plain step (not a jump) through.
/// @param dir     the step's direction
/// @param x       the target cell's column
/// @param y       its row
/// @param commit  the step is taken now (the rule may act: turn a gate); false for a check only
/// @return true to refuse the step (Blocked::Rule)
using StepRule = bool (*)(uint8_t dir, int x, int y, bool commit);

/// N2: sets the walker's step rule (nullptr: none, the default).
/// @param rule  the rule
void set_step_rule(StepRule rule);

/// Gives the walker a sprite slot to draw with (gfx::sprites::load() it
/// first), or none with @p slot = 0xFF.
/// @param slot    sprite slot
/// @param sprite  its asset (for its size and animations), or nullptr
void attach_sprite(uint8_t slot, const asset::Sprite* sprite);

/// Advances the walker one frame, moves the camera and positions the sprite.
/// Call once per frame, before gfx::sprites::update() and map::update().
/// @param intent  input for this frame
void update(const Intent& intent);

/// The walker.
/// @return its state
const Walker& walker();

/// Changes the walker's vehicle class (A2): its speeds, run permission,
/// animation set and passable terrain from the next step on. place() and
/// enter() keep it.
/// @param vehicle  0 to map::World::vehicle_class_count - 1 (asserted)
void set_vehicle(uint8_t vehicle);

/// Whether the walker may run now: its vehicle class may run and the shown
/// map allows it (map::kMapNoRun clear).
/// @return true when a run intent runs
bool can_run();

/// Turns debug noclip on or off.
/// @param on  walk through everything
void set_noclip(bool on);

/// Turns step encounters on or off (on by default).
/// @param on  roll on encounter cells
void set_encounters(bool on);

/// Collects the pending map event (B-3); the walker may move again
/// afterwards.
/// @param[out] out  the request, or nullptr to discard it
/// @return false when none is pending
bool take_event(FieldEvent* out);

/// Whether a map event is pending (the walker waits for take_event()).
/// @return true while one is
bool has_event();

/// Shows a map of the world with the walker on a cell (B-3): map::enter(),
/// place(), then, when a map was shown before (the same one too: a warp
/// within a map reloads it, as the reference games' do; or hidden by leave()), script::change_map() with both
/// area roots (B-2, so the map bank clears), then script::enter_map() (its
/// load scripts). The step counters, vehicle class and noclip carry over.
/// @param map     index into map::kWorld.maps
/// @param x       cell column
/// @param y       cell row
/// @param facing  Direction
void enter(uint16_t map, int x, int y, uint8_t facing);

/// C11: what enter_saved() lets the game do in the reference games' order (either may be nullptr).
struct SavedEntry
{
   void (*restore_view)();      ///< Before the load scripts: the saved view's cell edits back (map::set_cell()).
   void (*restore_objects)();   ///< After the objects spawn: the saved objects' cells, facings and removals.
};

/// C11: shows the map a saved game resumes on, as the reference games' Continue does (CB2_ContinueSavedGame:
/// InitMapFromSavedGame, then ResumeMap and InitObjectEventsReturnToField): as enter() but for its scripts:
/// map::enter() and place(), SavedEntry::restore_view, script::enter_saved_map() (on_load only: no on_transition),
/// on_resume, spawn_objects(), SavedEntry::restore_objects, then on_return (script::run_map_scripts() OnReturn)
/// instead of on_warp_into.
/// @param map     index into map::kWorld.maps
/// @param x       cell column
/// @param y       cell row
/// @param facing  Direction
/// @param hooks   the game's restore steps
void enter_saved(uint16_t map, int x, int y, uint8_t facing, const SavedEntry& hooks);

/// Hides the shown map (map::leave()) and its objects, between two maps (a
/// location preview, D-17). The next enter() still counts it as the map
/// left: script::change_map() runs with its area root.
void leave();

/// Shows another map under the walker without stopping it (I2, seamless connected maps): the new map's cells line
/// up with the shown map's shifted by (dx, dy) cells, so a cell (x, y) of the shown map is (x + dx, y + dy) of the
/// new one. The walker, the cell it is leaving and the camera move by the same amount, its step, facing, level and
/// vehicle class carry on, and the tile animations both maps run keep running (map::enter() keep_animations).
/// Then, as enter() does: script::change_map() with both area roots, script::enter_map(), the new map's objects.
/// @param map  index into map::kWorld.maps
/// @param dx   cells added to columns
/// @param dy   cells added to rows
void shift_map(uint16_t map, int dx, int dy);

/// Carries out a warp request: enter() at its landing cell, facing as now.
/// @param e  a FieldEvent with what == Request::Warp
void warp(const FieldEvent& e);

/// Collects the encounter the last step started; the walker may move
/// again afterwards.
/// @param[out] out  the encounter, or nullptr to discard it
/// @return false when no encounter is waiting
bool take_encounter(encounter::Result* out);

/// Checks one step without taking it.
/// @param x          from cell column
/// @param y          from cell row
/// @param d          Direction
/// @param elevation  the mover's level
/// @param vehicle    vehicle class
/// @param[out] jump  set to true when the step is a ledge jump (two cells)
/// @return Blocked::None when allowed
Blocked check_step(int x, int y, uint8_t d, uint8_t elevation, uint8_t vehicle, bool* jump);

/// @name Cutscene support (N2, D-32)
/// - The camera mover: spawn_camera() puts an invisible mover on the walker's cell and the camera follows it from
///   then on; routes given to kCameraObject (route_object(), the scripts' `route camera`) move it through anything at
///   the route's speed (object_moving(kCameraObject) while one runs). remove_camera() makes the camera follow the
///   walker again without moving the view: the offset the mover had from the walker stays until the next map (the
///   reference games' camera object; their scripts pan back before they remove it).
/// - Fixed draw order: set_fixed_order() keeps an object's (or the walker's) sprite at a sub-priority of its own
///   instead of the world's draw-order rule (gfx::sprites::set_order()).
/// - Sprite offsets: set_sprite_offset() draws an object's (or the walker's) sprite some pixels away from its cell
///   (a cutscene jostling a sprite; its cell, collision and draw order do not change).
/// - Effect sprite slots: claim_sprite_slot() hands out a sprite slot no map object uses (on a map whose objects
///   take fixed slots, one past them; with an object view, one of the free pool) for a game's effect sprites until
///   release_sprite_slot() or the next map (enter() and shift_map() take them back); objects that spawn later never
///   get a claimed slot.
/// None of these is saved; enter() and shift_map() reset them.
///@{
inline constexpr uint8_t kCameraObject = script::kObjectCamera;   ///< The camera mover's object operand.
inline constexpr uint8_t kNoSpriteSlot = 0xFF;   ///< claim_sprite_slot(): every slot is taken.

/// Spawns the camera mover on the walker's cell (again on its cell when it is already spawned).
void spawn_camera();

/// Removes the camera mover; the camera follows the walker again, keeping the view where it is.
void remove_camera();

/// Whether the camera mover is spawned.
/// @return true while it is
bool camera_spawned();

/// Gives an object's or the walker's sprite a fixed sub-priority, or the draw-order rule's again.
/// @param local  local ID, or script::kObjectPlayer
/// @param fixed  keep `order` (else the rule's)
/// @param order  0 (front) to 255
void set_fixed_order(uint8_t local, bool fixed, uint8_t order);

/// Draws an object's or the walker's sprite away from its cell.
/// @param local  local ID, or script::kObjectPlayer
/// @param dx     pixels right
/// @param dy     pixels down
void set_sprite_offset(uint8_t local, int16_t dx, int16_t dy);

/// Claims a sprite slot no map object uses (see above).
/// @return a slot (gfx/slots.h), or kNoSpriteSlot when every one is taken
uint8_t claim_sprite_slot();

/// Gives a claimed slot back and hides it.
/// @param slot  a slot claim_sprite_slot() gave
void release_sprite_slot(uint8_t slot);
///@}

/// Game hooks of the field (the game defines them; the engine's default has none).
namespace game
{

/// A due step hook (B-3): the game updates whatever the hook counts
/// (a counter, a timer, damage over steps, ...) and may ask for a script.
/// @param hook  hook ID (world_ids.h STEP_HOOK_...)
/// @return a script to run, or kNoScript
uint16_t step_hook(uint16_t hook);

/// I4: runs a game route step (capsule/script/route.h RouteOp::Game: the scripts' `step NAME` declarations, in
/// order; script_ids.h ROUTE_STEP_...). Called each frame the route stands on it until it is done. The default
/// (src/field/field_game_default.cpp, weak) is done at once.
/// @param mover  the object's local ID, or kNoObject for the walker
/// @param step   the step's ID
/// @param arg    its argument
/// @return true when it is done (the route goes on)
bool route_step(uint8_t mover, uint8_t step, uint8_t arg);

} // namespace game

} // namespace capsule::field
