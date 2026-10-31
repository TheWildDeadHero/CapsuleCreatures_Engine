/// @file
/// Overworld traversal (see capsule/field/field.h).
#include "capsule/field/field.h"
#include "capsule/field/doors.h"

#include "capsule/core/assert.h"
#include "capsule/core/endian.h"
#include "capsule/core/memory.h"
#include "capsule/core/random.h"
#include "capsule/core/trig.h"
#include "capsule/gfx/sprites.h"
#include "capsule/script/port.h"
#include "capsule/script/script.h"

namespace capsule::field
{
namespace
{

using route::RouteOp;

inline constexpr uint8_t kWalkerRoute = map::kMaxObjectTemplates;   ///< Route slot of the walker.
inline constexpr uint8_t kCameraRoute = kWalkerRoute + 1;   ///< N2: route slot of the camera mover.
inline constexpr uint8_t kRouteSlots = kCameraRoute + 1;    ///< Route slots: the objects', the walker's, the camera's.
inline constexpr int kRouteOpsPerFrame = 64;                 ///< Instant route steps run in one frame at most.

constexpr uint8_t kNoSlot = 0xFF;   ///< State::slot: the walker has no sprite slot.
constexpr uint8_t kNoAnim = 0xFF;   ///< State::anim / Object::anim: no animation playing (the next draw starts one).
constexpr uint8_t kDirectionMask = 3;   ///< Direction bits of a route step's direction byte.
constexpr uint32_t kCoinSides = 2;   ///< rng::below() outcomes of a coin flip.

/// @name Sprites on cells
///@{
constexpr int kCellCentre = map::kMetatilePixels / kHalf;   ///< Pixels from a cell's edge to its centre.
constexpr int kAnimSet = 4;   ///< Animations per set (one per direction): idle 0-3, walk 4-7, run 8-11.
constexpr int kWalkAnimFirst = 4;   ///< First walking animation.
constexpr int kRunAnimFirst = 8;   ///< First running animation.
constexpr int kAnimsWithRun = 12;   ///< Animations a sprite needs to have running ones.
///@}

/// @name Jumps
///@{
constexpr int kJumpCells = 2;   ///< Cells a ledge jump covers.
constexpr int kLedgeJumpPixels = kJumpCells * map::kMetatilePixels;   ///< Pixels of a ledge jump (its arc's length).
constexpr int kObjectJumpFrames = 16;   ///< Frames of an object's jump in place.
constexpr int kObjectJumpFramesPerCell = 8;   ///< Frames each cell adds to an object's jump.
constexpr int kJumpHeightDivisor = 8;   ///< Longer object jumps rise 1 pixel more per 8 frames.
///@}

/// @name Object movement timing (frequency levels, map::ObjectTemplate::motion)
///@{
constexpr uint8_t kMotionSpeedMask = 15;   ///< motion: speed bits.
constexpr int kMotionFrequencyShift = 4;   ///< motion: shift of the frequency.
constexpr uint8_t kRangeXMask = 15;   ///< range: cells each way horizontally.
constexpr int kRangeYShift = 4;   ///< range: shift of the vertical cells.
constexpr uint8_t kFrequencyLowest = 1;   ///< Frequency level: decisions 4 times as far apart.
constexpr uint8_t kFrequencyLow = 2;   ///< Frequency level: decisions twice as far apart.
constexpr uint8_t kFrequencyHigh = 4;   ///< Frequency level: decisions twice as often.
constexpr uint32_t kLowestScale = 4;   ///< Timer scale at kFrequencyLowest.
constexpr uint32_t kLowScale = 2;   ///< Timer scale at kFrequencyLow.
constexpr uint32_t kHighDivisor = 2;   ///< Timer divisor at kFrequencyHigh.
constexpr uint32_t kLookFrames = 32;   ///< LookAround / Wander: frames per pause unit.
constexpr uint32_t kLookPauseUnits = 4;   ///< LookAround / Wander: pauses of 1-4 units.
constexpr uint32_t kPaceFrames = 16;   ///< Pace / Approach: frames between steps.
constexpr int kFirstThinkFrames = 16;   ///< Least frames before an object's first decision.
constexpr int kFirstThinkStride = 13;   ///< Spreads first decisions by local ID...
constexpr int kFirstThinkSpread = 48;   ///< ...over this many frames.
///@}

/// @name Self switches
///@{
constexpr uint8_t kSelfSwitches = 4;   ///< Self switches per object (A-D).
constexpr int kByteOfBitShift = 3;   ///< Bit index to its byte (bit / 8).
constexpr int kBitInByteMask = 7;   ///< Bit index within its byte (bit & 7).
///@}

/// @name Route step sizes (bytes, opcode included)
///@{
constexpr uint16_t kStepOneByte = 2;   ///< Face, Speed, Frequency, Visible: one argument byte.
constexpr uint16_t kStepTwoBytes = 3;   ///< Move, Jump, Wait, Option: two argument bytes.
constexpr uint16_t kStepFlag = 4;   ///< Flag: flag16, on8.
constexpr uint16_t kStepSprite = 5;   ///< Sprite: key32.
constexpr int kSecondArg = 2;   ///< Offset of a step's second argument byte.
/// I4: frames one step takes at each speed level (0: walking), for RouteOp::InPlace.
constexpr uint8_t kStepFramesAt[route::kMaxSpeed + 1] = {16, 32, 16, 8, 4};
/// N2: the walk in place a RouteOp::InPlace step shows at each speed level (map::kInPlace...: slow, walk, jog, run).
constexpr uint8_t kInPlacePaceAt[route::kMaxSpeed + 1] = {map::kInPlaceWalk, map::kInPlaceSlow, map::kInPlaceWalk,
                                                          map::kInPlaceJog, map::kInPlaceRun};
constexpr int kFlagOnArg = 3;   ///< Flag: offset of its on byte.
///@}

constexpr int kBitsPerByte = 8;   ///< Bits of one byte (bit sets).

/// The machine's steps (Object::machine).
enum MachineStep : uint8_t
{
   kStart,      ///< Faces its facing (Turn / Roam: then draws a delay).
   kWait,       ///< Waits out its delay.
   kChoose,     ///< Turn / Roam / Rotate: chooses its next direction.
   kWalk,       ///< A step or a walk in place is under way.
   kIdle        ///< Nothing more to do (Invisible after its face).
};

constexpr uint8_t kNoPace = 0xFF;                         ///< Object::in_place_pace: a step, not a walk in place.

/// A move route being run (capsule/script/route.h).
struct RouteRun
{
   const uint8_t* code;   ///< The route (its flags byte first), or nullptr: none.
   uint16_t pc;   ///< Offset of the next step.
   uint16_t wait;   ///< Frames left of a Wait step.
   uint8_t left;   ///< Cells left of the current Move step (0: not started).
   bool script;   ///< Started by a script (routes_busy()).
   bool in_place;   ///< N2: the Wait under way is an InPlace step's walk in place (it ends with the wait).
};

/// Field state.
struct State
{
   Walker w;   ///< The walker.
   const asset::Sprite* sprite;   ///< Its sprite asset, or nullptr.
   uint8_t slot;   ///< Its sprite slot, or kNoSlot.
   uint8_t anim;   ///< Animation playing, or kNoAnim.
   bool encounters;   ///< Step encounters roll.
   bool has_encounter;   ///< `encounter` holds one not yet taken.
   encounter::Result encounter;   ///< The encounter the last step started.
   FieldEvent event;   ///< The pending map event (what == None: none).
   int16_t from_x;   ///< Cell the walker is leaving while it moves.
   int16_t from_y;   ///< Its row.
   uint16_t walker_steps;   ///< Scripted walker steps after the current one.
   uint8_t walker_dir;   ///< Their direction.
   bool scripted;   ///< The walker takes scripted steps (move_walker(), through anything).
   bool routed;   ///< The walker's current step comes from its route.
   bool locked;   ///< Objects do not move on their own (see release()).
   uint8_t approach;   ///< Spotter walking up to the walker, or kNoObject.
   uint8_t approach_timer;   ///< Frames before it starts walking.
   uint8_t object_count;   ///< Objects of the shown map.
   uint8_t frame;   ///< Frame counter (speed 1 moves on odd frames).
   uint32_t state_seen;   ///< script::state_version() when pages were last chosen.
   int32_t cam_dx;   ///< Camera offset from the walker (pans, shakes), pixels.
   int32_t cam_dy;   ///< Its row offset.
   uint16_t left_area;   ///< Area root of the map leave() hid (enter() clears it).
   bool left;   ///< leave() hid a map that enter() has not replaced yet.
   uint16_t slots_used;   ///< I3: object sprite slots in use (bit n = slot kFirstObjectSlot + n).
   uint8_t suppressed[map::kMaxObjectTemplates / kBitsPerByte];   ///< I3: objects show_object() hid (by local ID).
   bool view_due;   ///< I3: the walker started a step: objects spawn and go by the view after this frame's moves.
   bool bumping;   ///< I3: the walker's turn is a bump (a blocked step walked in place).
   uint8_t talked;   ///< I3: the object whose script was requested last (its machine restarts at release()).
   bool walker_began;   ///< I3: the walker began a step or a turn this frame (Mirror objects copy it).
   bool walker_began_walk;   ///< I3: that was a step (else a turn or a bump).
   uint8_t walker_began_dir;   ///< I3: its direction.
   /// @name N2: cutscene support (field.h "Cutscene support")
   ///@{
   bool camera_on;   ///< The camera mover is spawned.
   int16_t cam_x;   ///< Its cell's column (the destination while it moves).
   int16_t cam_y;   ///< Its row.
   int32_t cam_px;   ///< Left edge of its cell box, pixels.
   int32_t cam_py;   ///< Top edge, pixels.
   uint8_t cam_dir;   ///< Direction of its step under way.
   uint8_t cam_left;   ///< Pixels left of that step (0: standing).
   uint8_t cam_speed;   ///< Its route speed level.
   uint8_t cam_facing;   ///< Its facing (routes turn it).
   uint8_t cam_options;   ///< Its route options.
   int32_t cam_rest_dx;   ///< The view's offset a removed mover left, pixels.
   int32_t cam_rest_dy;   ///< Its row offset.
   bool walker_fixed;   ///< The walker's sprite keeps walker_order.
   uint8_t walker_order;   ///< That sub-priority.
   int16_t walker_dx;   ///< The walker's sprite offset, pixels right.
   int16_t walker_dy;   ///< Pixels down.
   bool walker_in_place;   ///< The walker walks in place (a route's InPlace step: its animation runs).
   uint8_t walker_speed;   ///< N2: the speed level of the walker's route steps (its Speed steps; route_walker()
                           ///< starts at route::kDefaultSpeed).
   uint16_t effect_slots;   ///< Slots claimed for effects (bit n = slot kFirstObjectSlot + n).
   ///@}
};

/// I3: the objects' state, by local ID (EWRAM: up to map::kMaxObjectTemplates per map).
CAPSULE_EWRAM_BSS Object g_objects[map::kMaxObjectTemplates];
/// Routes of the objects (by local ID), then the walker's (kWalkerRoute) and (N2) the camera mover's (kCameraRoute).
CAPSULE_EWRAM_BSS RouteRun g_routes[kRouteSlots];

/// I4: a change a script made to an object's template on the shown map (set_template_cell(),
/// set_template_movement()); the next map clears it.
struct TemplateOverride
{
   int16_t x;              ///< Its cell's column.
   int16_t y;              ///< Its row.
   bool cell;              ///< x and y replace the template's.
   bool movement;          ///< The movement fields replace the template's (and its pages').
   map::Movement kind;     ///< The movement.
   uint8_t arg;            ///< Its movement_arg.
   uint8_t flags;          ///< Its movement_flags.
   uint8_t facing;         ///< Its facing, or NUM_DIRECTIONS to keep the template's.
};

/// I4: the template changes, by local ID (EWRAM).
CAPSULE_EWRAM_BSS TemplateOverride g_overrides[map::kMaxObjectTemplates];

/// I4: forgets every template change (a new map).
void clear_overrides()
{
   for (TemplateOverride& o : g_overrides)
   {
      o = TemplateOverride{};
   }
}
/// The default walker (state that place() keeps is set there).
/// @return a walker with its defaults
constexpr Walker fresh_walker()
{
   Walker w{};
   w.visible = true;
   w.options = route::kDefaultOptions;
   return w;
}

/// The field at start-up: zero but for these defaults. Evaluated by the
/// compiler (no start-up constructor runs; the image is part of the data).
/// @return the initial state
constexpr State initial_state()
{
   State s{};
   s.w = fresh_walker();
   s.slot = kNoSlot;
   s.anim = kNoAnim;
   s.encounters = true;
   s.approach = kNoObject;
   s.talked = kNoObject;
   s.cam_speed = route::kDefaultSpeed;
   s.cam_options = route::kDefaultOptions;
   return s;
}

constinit State g = initial_state();   ///< The field.
constinit ActionRules g_action_rules = nullptr;   ///< F2: the field actions' dispatcher (set_action_rules()).
constinit StepRule g_step_rule = nullptr;   ///< N2: the game's step rule for the walker (set_step_rule()).

/// The shown map (asserted).
/// @return the map
const map::Map& shown()
{
   ENGINE_ASSERT(map::current() != nullptr);
   return *map::current();
}

/// Whether a cell can be entered (collision, elevation, terrain).
/// @param m          the map
/// @param x          cell column
/// @param y          cell row
/// @param elevation  mover's level
/// @param vehicle    vehicle class
/// @return Blocked::None when it can
Blocked can_enter(const map::Map& m, int x, int y, uint8_t elevation, uint8_t vehicle)
{
   const map::MapCell c = map::cell_at(m, x, y);
   if (map::cell_collision(c) != 0)
   {
      return Blocked::Collision;
   }
   if (!map::may_enter(elevation, map::cell_elevation(c)))
   {
      return Blocked::Elevation;
   }
   if ((map::behavior(c).passable & (1u << vehicle)) == 0)
   {
      return Blocked::Terrain;
   }
   return Blocked::None;
}

/// The opposite direction.
/// @param d  Direction
/// @return its opposite
constexpr uint8_t opposite(uint8_t d)
{
   return static_cast<uint8_t>(d ^ 1);
}
static_assert(opposite(DIR_DOWN) == DIR_UP && opposite(DIR_LEFT) == DIR_RIGHT);

/// The direction 90° clockwise (down -> left -> up -> right -> down).
/// @param d  Direction
/// @return turned
constexpr uint8_t turn_right(uint8_t d)
{
   return d == DIR_DOWN ? DIR_LEFT : (d == DIR_LEFT ? DIR_UP : (d == DIR_UP ? DIR_RIGHT : DIR_DOWN));
}

/// The direction 90° counter-clockwise.
/// @param d  Direction
/// @return turned
constexpr uint8_t turn_left(uint8_t d)
{
   return opposite(turn_right(d));
}
static_assert(turn_left(turn_right(DIR_UP)) == DIR_UP && turn_right(DIR_DOWN) == DIR_LEFT);

/// Pixels a mover at a speed level covers this frame.
/// @param level  1-4
/// @return 0-4 (level 1 moves on every other frame)
int pixels_at(uint8_t level)
{
   switch (level)
   {
   case route::kSpeedHalf:
      return (g.frame & 1) != 0 ? 1 : 0;
   case route::kSpeedDouble:
      return route::kSpeedDoublePixels;
   case route::kMaxSpeed:
      return route::kMaxSpeedPixels;
   default:
      return 1;
   }
}

/// The direction from one cell towards another (the longer axis; ties at random).
/// @param x   from column
/// @param y   from row
/// @param tx  to column
/// @param ty  to row
/// @return Direction, or NUM_DIRECTIONS on the same cell
uint8_t toward(int x, int y, int tx, int ty)
{
   const int dx = tx - x;
   const int dy = ty - y;
   if (dx == 0 && dy == 0)
   {
      return NUM_DIRECTIONS;
   }
   const int adx = dx < 0 ? -dx : dx;
   const int ady = dy < 0 ? -dy : dy;
   bool horizontal = adx > ady;
   if (adx == ady)
   {
      horizontal = rng::below(rng::gameplay, kCoinSides) == 0;
   }
   if (horizontal)
   {
      return static_cast<uint8_t>(dx < 0 ? DIR_LEFT : DIR_RIGHT);
   }
   return static_cast<uint8_t>(dy < 0 ? DIR_UP : DIR_DOWN);
}

/// Whether the walker is on a cell (or leaving it).
/// @param x  column
/// @param y  row
/// @return true when it is
bool walker_on(int x, int y)
{
   const Walker& w = g.w;
   return (w.x == x && w.y == y) ||
          (w.motion != Motion::Idle && w.motion != Motion::Turning && g.from_x == x && g.from_y == y);
}

/// The active object on a cell (standing there, moving there or leaving it).
/// @param x        column
/// @param y        row
/// @param level    the asker's level
/// @param except   an object to skip, or kNoObject
/// @param through  also find Through objects (which block nothing)
/// @return its local ID, or kNoObject
uint8_t object_on(int x, int y, uint8_t level, uint8_t except, bool through = false)
{
   for (uint8_t i = 0; i < g.object_count; ++i)
   {
      const Object& o = g_objects[i];
      if (!o.active || i == except || !map::levels_meet(o.elevation, level))
      {
         continue;
      }
      if (!through && (o.options & route::kOptionThrough) != 0)
      {
         continue;
      }
      if ((o.x == x && o.y == y) || (o.moving && o.from_x == x && o.from_y == y))
      {
         return i;
      }
   }
   return kNoObject;
}

/// Makes a request pending.
/// @param what    Script or Warp
/// @param source  why
/// @param script  script ID (Script)
/// @param x       event cell column
/// @param y       event cell row
void request(Request what, Source source, uint16_t script, int x, int y)
{
   g.event = FieldEvent{};
   g.event.what = what;
   g.event.source = source;
   g.event.script = script;
   g.event.at_x = static_cast<int16_t>(x);
   g.event.at_y = static_cast<int16_t>(y);
   g.event.object = kNoObject;
   if (what == Request::Script)
   {
      g.locked = true;
   }   // objects stand while the script runs
}

/// Requests an object's script.
/// @param i       local ID
/// @param source  why (Object, Touch, Autorun)
void request_object(uint8_t i, Source source)
{
   const Object& o = g_objects[i];
   request(Request::Script, source, o.script, o.x, o.y);
   g.event.object = i;
   g.talked = i;
}

/// Whether nothing else is going on, so an object's trigger may fire.
/// @return true when no request, encounter, approach, lock or scripted walk is pending
bool quiet()
{
   return g.event.what == Request::None && !g.has_encounter && g.approach == kNoObject && !g.locked && !g.scripted &&
          g_routes[kWalkerRoute].code == nullptr;
}

/// Whether an object's trigger is set off by touching (Player Touch, Event Touch).
/// @param o  the object
/// @return true for either
bool touch_trigger(const Object& o)
{
   return o.trigger == map::Trigger::PlayerTouch || o.trigger == map::Trigger::EventTouch;
}

/// Sets a facing unless direction fix is on.
/// @param facing   the facing to change
/// @param options  the mover's options
/// @param d        the new facing
void turn(uint8_t& facing, uint8_t options, uint8_t d)
{
   if ((options & route::kOptionDirFix) == 0 && d < NUM_DIRECTIONS)
   {
      facing = d;
   }
}

/// @name Step animations (I3)
/// A sprite with a step table (asset::Sprite::steps) animates as the reference games' movers do: standing, it shows
/// its walk animation for its facing paused on a standing entry (the first one when the animation was at or before
/// it, else the second); a step (or a walk in place) resumes the walk animation after the other standing entry, so
/// the feet alternate even across stops. Animation sets (capsule/field/field.h): + 4 walking, + 8 running, + 12 the
/// fast set (turning in place, jogging), + 16 the faster set.
///@{
constexpr int kTurnAnimFirst = 12;     ///< The fast set: turning in place, jogging in place.
constexpr int kFasterAnimFirst = 16;   ///< The faster set: running in place.
constexpr int kWalkAnimSet = 4;        ///< Walking, relative to a set's base (kWalkAnimFirst).

/// An animation a sprite has, or the walk set's (then the standing set's) for the direction.
/// @param s     the sprite
/// @param anim  the animation wanted
/// @param dir   its direction
/// @return an animation index the sprite has
int anim_or_walk(const asset::Sprite& s, int anim, uint8_t dir)
{
   if (anim < s.animation_count)
   {
      return anim;
   }
   if (kWalkAnimFirst + dir < s.animation_count)
   {
      return kWalkAnimFirst + dir;
   }
   return dir < s.animation_count ? dir : 0;
}

/// Starts or resumes a step animation (a step or a walk in place begins): the entry after the standing entry it
/// paused on, else where it is.
/// @param slot  the sprite slot
/// @param s     its sprite (with steps)
/// @param anim  the animation
void step_begin(uint8_t slot, const asset::Sprite& s, int anim)
{
   const uint8_t* t = s.steps;
   uint8_t e = gfx::sprites::entry(slot);
   if (e == t[asset::kStepFirstStand])
   {
      e = t[asset::kStepAfterFirst];
   }
   else if (e == t[asset::kStepSecondStand])
   {
      e = t[asset::kStepAfterSecond];
   }
   gfx::sprites::play_at(slot, static_cast<uint16_t>(anim), e);
   gfx::sprites::pause(slot, false);
   gfx::sprites::set_half_speed(slot, false);
}

/// Shows a standing mover: its walk animation for the facing, paused on a standing entry.
/// @param slot  the sprite slot
/// @param s     its sprite (with steps)
/// @param anim  the walk animation
void step_face(uint8_t slot, const asset::Sprite& s, int anim)
{
   const uint8_t* t = s.steps;
   const uint8_t now = gfx::sprites::entry(slot);
   const uint8_t e = now <= t[asset::kStepFirstStand] ? t[asset::kStepFirstStand] : t[asset::kStepSecondStand];
   if (gfx::sprites::animation(slot) != anim || now != e)
   {
      gfx::sprites::play_at(slot, static_cast<uint16_t>(anim), e);
   }
   gfx::sprites::pause(slot, true);
   gfx::sprites::set_half_speed(slot, false);
}

/// The animation of an object's walk in place for its pace.
/// @param s     its sprite
/// @param pace  map::kInPlace...
/// @param dir   its facing
/// @return the animation
int in_place_anim(const asset::Sprite& s, uint8_t pace, uint8_t dir)
{
   switch (pace)
   {
   case map::kInPlaceJog:
      return anim_or_walk(s, kTurnAnimFirst + dir, dir);
   case map::kInPlaceRun:
      return anim_or_walk(s, kFasterAnimFirst + dir, dir);
   default:
      return anim_or_walk(s, kWalkAnimFirst + dir, dir);
   }
}

/// @cond FORWARD
void draw_step_object(uint8_t i);   // documented below
/// @endcond
///@}

/// @name The walker
///@{

/// I3: starts the walker's step animation (a step sprite: the set's animation for its facing resumes after the
/// standing entry it paused on).
/// @param set  kWalkAnimFirst, kRunAnimFirst or kTurnAnimFirst (relative to its vehicle class's first animation)
void walker_step_anim(int set)
{
   if (g.slot == kNoSlot || g.sprite == nullptr || g.sprite->steps == nullptr)
   {
      return;
   }
   const int base = map::vehicle_class(g.w.vehicle_class).first_animation;
   step_begin(g.slot, *g.sprite, anim_or_walk(*g.sprite, base + set + g.w.facing, g.w.facing));
   g.anim = kNoAnim;
}

/// I3: notes that the walker began a step or a turn this frame (Mirror objects copy it).
/// @param walk  a step (else a turn or a bump)
/// @param d     its direction
void began(bool walk, uint8_t d)
{
   g.walker_began = true;
   g.walker_began_walk = walk;
   g.walker_began_dir = d;
}

/// Starts a move.
/// @param d       direction
/// @param cells   1, or 2 for a jump
/// @param speed   pixels per frame
/// @param motion  Walking, Jumping, Sliding or Pushed
void start_move(uint8_t d, int cells, uint8_t speed, Motion motion)
{
   Walker& w = g.w;
   g.from_x = w.x;
   g.from_y = w.y;
   w.move_dir = d;
   w.x = static_cast<int16_t>(w.x + dx_of(d) * cells);
   w.y = static_cast<int16_t>(w.y + dy_of(d) * cells);
   w.speed = speed;
   w.remaining = static_cast<uint8_t>(map::kMetatilePixels * cells);
   w.motion = motion;
   w.last_blocked = Blocked::None;
   g.view_due = true;   // I3: the camera moves on: objects spawn and go by the view
   walker_step_anim(motion == Motion::Walking && w.running ? kRunAnimFirst : kWalkAnimFirst);
}

/// Tries a chosen or forced step.
/// @param d       direction
/// @param speed   pixels per frame (jumps use kRunSpeed)
/// @param motion  motion for a plain step
/// @param touch   a blocking Touch object fires (player input)
/// @return true if the walker moves
bool try_step(uint8_t d, uint8_t speed, Motion motion, bool touch = false)
{
   Walker& w = g.w;
   bool jump = false;
   const bool through = w.noclip || (w.options & route::kOptionThrough) != 0;
   Blocked b = through ? Blocked::None : check_step(w.x, w.y, d, w.elevation, w.vehicle_class, &jump);
   // F2: a class that dismounts takes a step only class 0 may take as class 0 (leaving the water after crossing it)
   bool dismount = false;
   if (b == Blocked::Terrain && w.vehicle_class != 0 &&
       (map::vehicle_class(w.vehicle_class).flags & map::kVehicleDismount) != 0)
   {
      bool foot_jump = false;
      if (check_step(w.x, w.y, d, w.elevation, 0, &foot_jump) == Blocked::None)
      {
         dismount = true;
         jump = foot_jump;
         b = Blocked::None;
      }
   }
   uint8_t blocker = kNoObject;
   if (b == Blocked::None && !through)
   {
      const int cells = jump ? kJumpCells : 1;
      for (int k = 1; k <= cells && blocker == kNoObject; ++k)
      {
         blocker = object_on(w.x + dx_of(d) * k, w.y + dy_of(d) * k, w.elevation, kNoObject);
      }
      if (blocker != kNoObject)
      {
         b = Blocked::Object;
      }
   }
   // N2: the game's rule last, for a plain step the cell and the objects let through
   if (b == Blocked::None && !through && !jump && g_step_rule != nullptr &&
       g_step_rule(d, w.x + dx_of(d), w.y + dy_of(d), true))
   {
      b = Blocked::Rule;
   }
   w.last_blocked = b;
   if (b != Blocked::None)
   {
      if (touch && blocker != kNoObject && touch_trigger(g_objects[blocker]) &&
          g_objects[blocker].script != kNoScript && quiet())
      {
         request_object(blocker, Source::Touch);
      }
      return false;
   }
   if (dismount)
   {
      set_vehicle(0);
      speed = map::vehicle_class(0).walk_speed;   // the step off is taken on foot
   }
   if (jump)
   {
      start_move(d, kJumpCells, kRunSpeed, Motion::Jumping);
   }
   else
   {
      start_move(d, 1, speed, motion);
   }
   return true;
}

///@}

/// @name Objects (B-21, D-17)
///@{

/// An object's template.
/// @param i  local ID
/// @return it
const map::ObjectTemplate& tmpl(uint8_t i)
{
   return shown().objects[i];
}

/// Positions and animates an object's sprite (hidden off screen, so OAM
/// coordinates never wrap it back on).
/// @param i  local ID
void draw_object(uint8_t i)
{
   const Object& o = g_objects[i];
   const uint8_t slot = o.slot;
   if (slot == kNoSlot)
   {
      return;   // I3: not spawned (by the view): no sprite slot
   }
   if (!o.active || !o.visible || o.invisible || o.sprite == nullptr)
   {
      gfx::sprites::show(slot, false);
      return;
   }
   const int sw = hal::video::obj_width(o.sprite->shape, o.sprite->size);
   const int sh = hal::video::obj_height(o.sprite->shape, o.sprite->size);
   // Centred on its cell, standing on the cell's bottom edge (N2: moved by its sprite offset).
   const int32_t sx = o.px + kCellCentre - sw / kHalf - map::camera_x() + o.sprite_dx;
   const int32_t sy = o.py + map::kMetatilePixels - sh - o.lift - map::camera_y() + o.sprite_dy;
   const bool on = sx > -sw && sx < hal::video::kScreenWidth && sy > -sh && sy < hal::video::kScreenHeight;
   gfx::sprites::show(slot, on);
   if (!on)
   {
      return;
   }
   gfx::sprites::set_position(slot, static_cast<int16_t>(sx), static_cast<int16_t>(sy));
   const uint8_t pr = (o.options & route::kOptionOnTop) != 0 ? uint8_t{0} : map::elevation(o.elevation).priority;
   gfx::sprites::set_priority(slot, pr);
   // DS1: its place among the sprites, from its bottom edge on screen (the lift and N2's offset left out) and its
   // level; N2: or the sub-priority a script fixed
   gfx::sprites::set_order(slot, o.fixed_order ? o.order : map::sub_priority(sy + sh + o.lift - o.sprite_dy,
                                                                             o.elevation));
   if (o.sprite->steps != nullptr)
   {
      draw_step_object(i);   // I3: step animations (the walk animation paused between steps)
      return;
   }
   const bool walking =
      (o.moving && (o.options & route::kOptionWalkAnim) != 0) || (o.options & route::kOptionStepAnim) != 0;
   uint8_t anim = static_cast<uint8_t>(walking ? kWalkAnimFirst + o.facing : o.facing);
   if (anim >= o.sprite->animation_count)
   {   // fall back to the idle set
      anim = anim % kAnimSet < o.sprite->animation_count ? anim % kAnimSet : 0;
   }
   if (anim != o.anim)
   {
      g_objects[i].anim = anim;
      gfx::sprites::play(slot, anim);
   }
}

/// I3: animates a standing object with a step sprite: its walk animation paused on a standing entry (moving objects
/// and walks in place set their animation when they start).
/// @param i  local ID
void draw_step_object(uint8_t i)
{
   const Object& o = g_objects[i];
   if (o.moving || o.in_place > 0 || o.in_place_pace != kNoPace || o.movement == map::Movement::InPlace)
   {
      return;
   }
   step_face(o.slot, *o.sprite, anim_or_walk(*o.sprite, kWalkAnimFirst + o.facing, o.facing));
}

/// Changes an object's sprite (loaded into its slot when it differs).
/// @param i    local ID
/// @param key  the asset key
void set_sprite(uint8_t i, AssetKey key)
{
   Object& o = g_objects[i];
   if (o.sprite != nullptr && o.sprite_key == key)
   {
      return;
   }
   o.sprite_key = key;
   o.sprite = asset::sprite(key);
   o.anim = kNoAnim;
   if (o.sprite != nullptr && o.slot != kNoSlot)
   {
      gfx::sprites::load(o.slot, *o.sprite);
   }
}

/// The bit of an object's self switch.
/// @param i       local ID
/// @param letter  0-3
/// @return bit index, or -1 when it has none
int32_t self_bit(uint8_t i, uint8_t letter)
{
   if (i >= g.object_count || letter >= kSelfSwitches)
   {
      return -1;
   }
   const uint16_t base = tmpl(i).self_base;
   if (base == map::kNoSelfSwitches || map::kWorld.self_switches == nullptr)
   {
      return -1;
   }
   return int32_t{base} + letter;
}

/// Whether an object's page's conditions hold.
/// @param i  local ID
/// @param p  the page
/// @return true when they all do
bool page_holds(uint8_t i, const map::ObjectPage& p)
{
   if (p.flag != map::kNoEventCondition && !script::flag(script::FlagId{p.flag}))
   {
      return false;
   }
   if (p.var != map::kNoEventCondition)
   {
      const int32_t v = script::var(script::VarId{p.var});
      if ((p.cond & map::kPageVarEqual) != 0 ? v != p.value : v < p.value)
      {
         return false;
      }
   }
   for (uint8_t letter = 0; letter < kSelfSwitches; ++letter)
   {
      if ((p.self_mask & (1u << letter)) != 0 && !self_switch(i, letter))
      {
         return false;
      }
   }
   return true;
}

/// An object's active page.
/// @param i  local ID
/// @return 0 (its own fields) or 1 + the index among its pages
uint8_t choose_page(uint8_t i)
{
   const map::Map& m = shown();
   const map::ObjectTemplate& t = m.objects[i];
   for (int k = t.page_count; k > 0; --k)
   {
      ENGINE_ASSERT(m.object_pages != nullptr && t.page_first + k - 1 < m.object_page_count);
      if (page_holds(i, m.object_pages[t.page_first + k - 1]))
      {
         return static_cast<uint8_t>(k);
      }
   }
   return 0;
}

/// Starts a route on a route slot.
/// @param slot    object local ID, or kWalkerRoute
/// @param code    the route, or nullptr to stop
/// @param script  started by a script
void start_route(uint8_t slot, const uint8_t* code, bool script)
{
   RouteRun& r = g_routes[slot];
   if (r.in_place)
   {   // N2: a walk in place the old route left under way ends (the next draw shows the mover standing)
      if (slot < kWalkerRoute)
      {
         g_objects[slot].in_place_pace = kNoPace;
      }
      else if (slot == kWalkerRoute)
      {
         g.walker_in_place = false;
      }
   }
   r = RouteRun{};
   r.code = code;
   r.pc = 1;   // after the flags byte
   r.script = script;
}

/// The route of a map's route table.
/// @param index  route index, or map::kNoRoute
/// @return its code, or nullptr
const uint8_t* map_route(uint16_t index)
{
   const map::Map& m = shown();
   if (index == map::kNoRoute || index >= m.route_count || m.route_code == nullptr)
   {
      return nullptr;
   }
   return m.route_code + m.route_offsets[index];
}

/// Makes a page an object's active one: its fields, a fresh movement and facing.
/// @param i     local ID
/// @param page  0 or 1 + page index
void apply_page(uint8_t i, uint8_t page)
{
   const map::Map& m = shown();
   const map::ObjectTemplate& t = m.objects[i];
   Object& o = g_objects[i];
   AssetKey sprite = t.sprite;
   o.script = t.script;
   o.movement = t.movement;
   o.movement_arg = t.movement_arg;
   o.movement_flags = t.movement_flags;
   o.range = t.range;
   o.route = t.route;
   o.base_facing = t.facing < NUM_DIRECTIONS ? t.facing : static_cast<uint8_t>(DIR_DOWN);
   o.trigger = t.trigger;
   uint8_t motion = t.motion;
   o.options = t.options;
   bool hidden = false;
   if (page > 0)
   {
      const map::ObjectPage& p = m.object_pages[t.page_first + page - 1];
      if (p.fields & map::kPageSprite)
      {
         sprite = p.sprite;
      }
      if (p.fields & map::kPageScript)
      {
         o.script = p.script;
      }
      if (p.fields & map::kPageMovement)
      {
         o.movement = p.movement;
         o.movement_arg = p.movement_arg;
         o.movement_flags = p.movement_flags;
         o.range = p.range;
         o.route = p.route;
      }
      if (p.fields & map::kPageFacing)
      {
         o.base_facing = p.facing;
      }
      if (p.fields & map::kPageTrigger)
      {
         o.trigger = p.trigger;
      }
      if (p.fields & map::kPageMotion)
      {
         motion = p.motion;
      }
      if (p.fields & map::kPageOptions)
      {
         o.options = p.options;
      }
      hidden = (p.fields & map::kPageHidden) != 0;
   }
   o.speed = static_cast<uint8_t>(motion & kMotionSpeedMask);
   o.frequency = static_cast<uint8_t>(motion >> kMotionFrequencyShift);
   if (o.speed < 1 || o.speed > route::kMaxSpeed)
   {
      o.speed = route::kDefaultSpeed;
   }
   if (o.frequency < 1 || o.frequency > route::kMaxFrequency)
   {
      o.frequency = route::kDefaultFrequency;
   }
   const TemplateOverride& over = g_overrides[i];
   if (over.movement)
   {   // I4: a script changed the template's movement (it holds for every page)
      o.movement = over.kind;
      o.movement_arg = over.arg;
      o.movement_flags = over.flags;
      if (over.facing < NUM_DIRECTIONS)
      {
         o.base_facing = over.facing;
      }
   }
   o.page = hidden ? kHiddenPage : page;
   o.active = o.present && !hidden;
   if (!o.moving)
   {
      o.facing = o.base_facing;
   }
   // Spread the first decisions.
   o.timer = static_cast<uint16_t>(kFirstThinkFrames + (i * kFirstThinkStride) % kFirstThinkSpread);
   o.visible = true;
   // I3: a new page starts its movement machine over (the walker's facing is taken again by a Mirror)
   o.machine = kStart;
   o.sequence = 0;
   o.delay = 0;
   o.in_place = 0;
   o.in_place_pace = kNoPace;
   o.invisible = false;
   set_sprite(i, sprite);
   if (!o.active)
   {
      g_routes[i] = RouteRun{};   // hidden: a script waiting on its route (wait_moves) goes on
      o.steps_left = 0;
   }
   else if (!g_routes[i].script)
   {
      start_route(i, o.movement == map::Movement::Custom ? map_route(o.route) : nullptr, false);
   }
   if (!o.active && g.approach == i)
   {
      g.approach = kNoObject;
      if (g.event.what == Request::None)
      {
         g.locked = false;
      }
   }
   draw_object(i);
}

/// Chooses every present object's page again (flags, variables or self
/// switches changed); objects whose page changes take it.
void refresh_pages()
{
   g.state_seen = script::state_version();
   if (map::current() == nullptr)
   {
      return;
   }
   for (uint8_t i = 0; i < g.object_count; ++i)
   {
      if (!g_objects[i].present)
      {
         continue;
      }
      const uint8_t page = choose_page(i);
      const map::ObjectPage* p = page > 0 ? &shown().object_pages[tmpl(i).page_first + page - 1] : nullptr;
      const uint8_t now = (p != nullptr && (p->fields & map::kPageHidden) != 0) ? kHiddenPage : page;
      if (now != g_objects[i].page)
      {
         apply_page(i, page);
      }
   }
}

/// C12: an object's template column as the shown map has it now: a script's move (set_template_cell()) or the
/// compiled template's.
/// @param i  local ID
/// @return the column
int16_t template_x(uint8_t i)
{
   const TemplateOverride& over = g_overrides[i];
   return over.cell ? over.x : tmpl(i).x;
}

/// C12: its row (template_x()).
/// @param i  local ID
/// @return the row
int16_t template_y(uint8_t i)
{
   const TemplateOverride& over = g_overrides[i];
   return over.cell ? over.y : tmpl(i).y;
}

/// Spawns one object at its template cell (template_x()) with its active page.
/// @param i  local ID
void spawn(uint8_t i)
{
   const map::ObjectTemplate& t = tmpl(i);
   Object& o = g_objects[i];
   const uint8_t slot = o.slot;   // given by spawn_objects() / the view
   o = Object{};
   o.slot = slot;
   o.present = true;
   o.elevation = t.elevation;
   const int16_t tx = template_x(i);   // I4: a script may have moved its template
   const int16_t ty = template_y(i);
   o.x = o.from_x = o.home_x = tx;
   o.y = o.from_y = o.home_y = ty;
   o.px = static_cast<int32_t>(tx) * map::kMetatilePixels;
   o.py = static_cast<int32_t>(ty) * map::kMetatilePixels;
   o.anim = kNoAnim;
   g_routes[i] = RouteRun{};
   apply_page(i, choose_page(i));
   if (o.sprite != nullptr && o.sprite->steps != nullptr && o.slot != kNoSlot)
   {   // I3: a spawned mover shows its standing animation first (its machine's face action takes over)
      gfx::sprites::play(o.slot, o.facing < o.sprite->animation_count ? o.facing : uint8_t{0});
   }
}

/// @name Spawning by the view (I3, map::ObjectView)
///@{

/// Whether a cell is inside the shown map's object view around the walker.
/// @param x  column
/// @param y  row
/// @return true when it is
bool in_view(int x, int y)
{
   const map::ObjectView& v = shown().object_view;
   const int dx = x - g.w.x;
   const int dy = y - g.w.y;
   return dx >= -v.left && dx <= v.right && dy >= -v.up && dy <= v.down;
}

/// Whether show_object() hid an object (it does not come back by the view until shown or the next map).
/// @param i  local ID
/// @return true when it did
bool suppressed(uint8_t i)
{
   return ((g.suppressed[i / kBitsPerByte] >> (i % kBitsPerByte)) & 1u) != 0;
}

/// Marks or clears an object as hidden by show_object().
/// @param i   local ID
/// @param on  hidden
void set_suppressed(uint8_t i, bool on)
{
   const auto bit = static_cast<uint8_t>(1u << (i % kBitsPerByte));
   uint8_t& byte = g.suppressed[i / kBitsPerByte];
   byte = static_cast<uint8_t>(on ? (byte | bit) : (byte & ~bit));
}

/// A sprite slot for an object: its own (slot 1 + local ID) on a map without a view, else the first free one.
/// @param i  local ID
/// @return the slot, or kNoSlot when every object slot is taken
uint8_t claim_slot(uint8_t i)
{
   if (!map::spawns_by_view(shown()))
   {
      return static_cast<uint8_t>(kFirstObjectSlot + i);
   }
   for (uint8_t n = 0; n < map::kMaxObjects; ++n)
   {
      if ((g.slots_used >> n & 1u) == 0)
      {
         g.slots_used = static_cast<uint16_t>(g.slots_used | 1u << n);
         return static_cast<uint8_t>(kFirstObjectSlot + n);
      }
   }
   return kNoSlot;
}

/// Gives an object's sprite slot back (a map with a view) and hides it.
/// @param i  local ID
void free_slot(uint8_t i)
{
   Object& o = g_objects[i];
   if (o.slot == kNoSlot)
   {
      return;
   }
   gfx::sprites::show(o.slot, false);
   if (map::spawns_by_view(shown()))
   {
      g.slots_used = static_cast<uint16_t>(g.slots_used & ~(1u << (o.slot - kFirstObjectSlot)));
      o.slot = kNoSlot;
   }
}

/// Takes an object off the map (out of view, or hidden by a call): its route, approach and slot go.
/// @param i  local ID
void despawn(uint8_t i)
{
   Object& o = g_objects[i];
   o.present = false;
   o.active = false;
   o.moving = false;
   g_routes[i] = RouteRun{};
   if (g.approach == i)
   {   // its approach ends with no script: nothing holds the lock
      g.approach = kNoObject;
      if (g.event.what == Request::None)
      {
         g.locked = false;
      }
   }
   draw_object(i);
   free_slot(i);
}

/// Spawns the objects whose template cells (template_x(): a script's move included) came into the view and takes off
/// those whose cell and spawn cell (Object::home_x) both left it (spawns first: a slot freed now is free from the
/// next update on).
void update_view()
{
   const map::Map& m = shown();
   for (uint8_t i = 0; i < g.object_count; ++i)
   {
      const map::ObjectTemplate& t = m.objects[i];
      if (g_objects[i].present || suppressed(i) || !in_view(template_x(i), template_y(i)) ||
          (t.flag != map::kNoEventCondition && script::flag(script::FlagId{t.flag})))
      {
         continue;
      }
      const uint8_t slot = claim_slot(i);
      if (slot == kNoSlot)
      {
         break;   // every slot is taken: the rest wait for the next look
      }
      g_objects[i].slot = slot;
      spawn(i);
   }
   for (uint8_t i = 0; i < g.object_count; ++i)
   {
      const Object& o = g_objects[i];
      if (o.present && !in_view(o.x, o.y) && !in_view(o.home_x, o.home_y))
      {
         despawn(i);
      }
   }
}
///@}

/// C10: an object's level after a step: the arrival cell's, unless that cell keeps the mover's level (a bridge's
/// multi-level cells), as the walker's (map::arrival_level()); its sprite's priority and draw order follow it.
/// @param o  the object, standing on its arrival cell
void arrive_level(Object& o)
{
   o.elevation = map::arrival_level(o.elevation, map::cell_elevation(map::cell_at(shown(), o.x, o.y)));
}

/// C10: on a map without a view, spawns the objects that are off the map but not held hidden (remove_object())
/// once their hide flag is clear (the look a map with a view makes in update_view()).
void respawn_cleared()
{
   const map::Map& m = shown();
   for (uint8_t i = 0; i < g.object_count; ++i)
   {
      const map::ObjectTemplate& t = m.objects[i];
      if (g_objects[i].present || suppressed(i) ||
          (t.flag != map::kNoEventCondition && script::flag(script::FlagId{t.flag})))
      {
         continue;
      }
      if (g_objects[i].slot == kNoSlot)
      {
         g_objects[i].slot = claim_slot(i);
      }
      spawn(i);
   }
}

/// Starts one step of an object.
/// @param o  the object
/// @param d  Direction
void begin_step(Object& o, uint8_t d)
{
   o.move_dir = d;
   turn(o.facing, o.options, d);
   o.from_x = o.x;
   o.from_y = o.y;
   o.x = static_cast<int16_t>(o.x + dx_of(d));
   o.y = static_cast<int16_t>(o.y + dy_of(d));
   o.remaining = map::kMetatilePixels;
   o.moving = true;
   if (o.sprite != nullptr && o.sprite->steps != nullptr && o.slot != kNoSlot)
   {   // I3: the walk animation resumes after the standing entry it paused on
      step_begin(o.slot, *o.sprite, anim_or_walk(*o.sprite, kWalkAnimFirst + o.facing, o.facing));
   }
}

/// Whether an object may enter a cell: passable (no ledges), free of the
/// walker and other objects; Through objects may always.
/// @param i     local ID
/// @param nx    column
/// @param ny    row
/// @return true when it may
bool may_enter(uint8_t i, int nx, int ny)
{
   const Object& o = g_objects[i];
   if ((o.options & route::kOptionThrough) != 0)
   {
      return true;
   }
   const map::Map& m = shown();
   if (can_enter(m, nx, ny, o.elevation, 0) != Blocked::None)
   {
      return false;
   }
   if (map::behavior(map::cell_at(m, nx, ny)).kind == map::BehaviorKind::Ledge)
   {
      return false;
   }
   // I3: walls (one-way fences) stop objects as they stop the walker: a step to a neighbouring cell
   const int ddx = nx - o.x;
   const int ddy = ny - o.y;
   if ((ddx == 0) != (ddy == 0) && ddx >= -1 && ddx <= 1 && ddy >= -1 && ddy <= 1)
   {
      const uint8_t d = ddx < 0 ? DIR_LEFT : (ddx > 0 ? DIR_RIGHT : (ddy < 0 ? DIR_UP : DIR_DOWN));
      if (map::walls_block(map::behavior(map::cell_at(m, o.x, o.y)), map::behavior(map::cell_at(m, nx, ny)), d))
      {
         return false;
      }
   }
   const bool walker_through = (g.w.options & route::kOptionThrough) != 0;
   return (walker_through || !walker_on(nx, ny)) && object_on(nx, ny, o.elevation, i) == kNoObject;
}

/// An Event Touch object blocked by the walker fires.
/// @param i   local ID
/// @param nx  the cell it tried
/// @param ny  its row
void touch_walker(uint8_t i, int nx, int ny)
{
   const Object& o = g_objects[i];
   if (o.trigger == map::Trigger::EventTouch && o.script != kNoScript && walker_on(nx, ny) && quiet())
   {
      request_object(i, Source::Touch);
   }
}

/// Tries one step of an object on its own (or by a route).
/// @param i      local ID
/// @param d      Direction
/// @param boxed  only inside its range box
/// @return true when it moves
bool try_object_step(uint8_t i, uint8_t d, bool boxed)
{
   Object& o = g_objects[i];
   const int nx = o.x + dx_of(d);
   const int ny = o.y + dy_of(d);
   if (boxed)
   {   // C12: the range box is around the spawn cell
      const int rx = o.range & kRangeXMask;
      const int ry = o.range >> kRangeYShift;
      if (nx < o.home_x - rx || nx > o.home_x + rx || ny < o.home_y - ry || ny > o.home_y + ry)
      {
         return false;
      }
   }
   if (!may_enter(i, nx, ny))
   {
      touch_walker(i, nx, ny);
      return false;
   }
   begin_step(o, d);
   return true;
}

/// Starts a jump of an object (in place when both offsets are 0).
/// @param i   local ID
/// @param dx  cells right
/// @param dy  cells down
/// @return true when it jumps
bool try_object_jump(uint8_t i, int dx, int dy)
{
   Object& o = g_objects[i];
   const int nx = o.x + dx;
   const int ny = o.y + dy;
   if ((dx != 0 || dy != 0) && !may_enter(i, nx, ny))
   {
      return false;
   }
   const int dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
   o.from_x = o.x;
   o.from_y = o.y;
   o.x = static_cast<int16_t>(nx);
   o.y = static_cast<int16_t>(ny);
   if (dx != 0 || dy != 0)
   {
      const uint8_t d = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy) ? static_cast<uint8_t>(dx < 0 ? DIR_LEFT : DIR_RIGHT)
                                                                   : static_cast<uint8_t>(dy < 0 ? DIR_UP : DIR_DOWN);
      turn(o.facing, o.options, d);
   }
   o.move_dir = NUM_DIRECTIONS;   // a jump: remaining counts frames
   o.remaining = static_cast<uint8_t>(kObjectJumpFrames + kObjectJumpFramesPerCell * dist);
   o.jump_frames = o.remaining;   // the jump's length, for the arc
   o.moving = true;
   return true;
}

/// Scales a decision timer by an object's frequency.
/// @param o      the object
/// @param frames timer at frequency 3
/// @return the timer
uint16_t at_frequency(const Object& o, uint32_t frames)
{
   switch (o.frequency)
   {
   case kFrequencyLowest:
      return static_cast<uint16_t>(frames * kLowestScale);
   case kFrequencyLow:
      return static_cast<uint16_t>(frames * kLowScale);
   case kFrequencyHigh:
      return static_cast<uint16_t>(frames / kHighDivisor);
   case route::kMaxFrequency:
      return 0;   // acts at once
   default:
      return static_cast<uint16_t>(frames);
   }
}

/// @cond FORWARD
void run_route(uint8_t slot);   // documented with the Routes group
/// @endcond

/// @cond FORWARD
void advance_object(uint8_t i);   // documented with the objects' movement
/// @endcond

/// @name The movements I3 adds (map.h "Movement arguments")
/// Each runs a small machine once a frame while its object is not moving on its own and objects are not locked;
/// several of its steps may run in one frame, as the reference games' movement types do. A face action takes no
/// frame; a step moves its first pixel in the frame it starts (16 frames a cell); a walk in place lasts its pace's
/// frames. Random draws (rng::gameplay, each a call to rng::next16(), as the reference games call Random()): Turn: a delay after each face, a direction
/// before it (unless a runner is watched); Roam: a delay after each face, a direction when it ends; Rotate,
/// Patrol, Circuit, Mirror, InPlace and Invisible draw none.
///@{
constexpr uint16_t kDelaysMedium[] = {32, 64, 96, 128};   ///< map::kMoveDelaysMedium.
constexpr uint16_t kDelaysShort[] = {32, 48, 64, 80};     ///< map::kMoveDelaysShort.
constexpr uint32_t kDelayChoices = 4;                     ///< Entries of a delay table.
constexpr uint32_t kFourChoicesMask = 3;                  ///< A draw's bits choosing among four.
constexpr uint32_t kTwoChoicesMask = 1;                   ///< A draw's bits choosing among two.
constexpr uint8_t kTwoChoices = 2;                        ///< Entries a two-way choice table uses.
constexpr uint8_t kLegs = 4;                              ///< Legs of a circuit.
constexpr uint8_t kLastLeg = kLegs - 1;                   ///< The circuit's last leg.
constexpr uint8_t kLegMask = 3;                           ///< One leg's (or one choice's) direction bits.
constexpr uint8_t kRangeYShiftOf = 4;                     ///< range: shift of the vertical cells.
constexpr uint8_t kRangeXOf = 15;                         ///< range: the horizontal cells.
/// Frames of a walk in place, by pace (map::kInPlace...).
constexpr uint8_t kInPlaceFrames[] = {32, 16, 8, 4};


/// What lies in a direction for these movements (the reference games' object collision).
enum class Way : uint8_t
{
   Free,           ///< It may step there.
   OutsideRange,   ///< The cell is outside its range box.
   Blocked         ///< Collision, a wall, another level, the walker or another object.
};

/// Whether two movers' levels make them block each other, as the reference games' objects do (0 meets every
/// level; otherwise they must be equal).
/// @param a  a level
/// @param b  another
/// @return true when they do
constexpr bool levels_compatible(uint8_t a, uint8_t b)
{
   return a == 0 || b == 0 || a == b;
}

/// What lies in a direction for an object's movement machine.
/// @param i  local ID
/// @param d  Direction
/// @return Free, OutsideRange or Blocked
Way way_for(uint8_t i, uint8_t d)
{
   const Object& o = g_objects[i];
   const int nx = o.x + dx_of(d);
   const int ny = o.y + dy_of(d);
   // a range of 0 on an axis leaves it unbounded; C12: the range is around the spawn cell
   const int rx = o.range & kRangeXOf;
   const int ry = o.range >> kRangeYShiftOf;
   if ((rx != 0 && (nx < o.home_x - rx || nx > o.home_x + rx)) ||
       (ry != 0 && (ny < o.home_y - ry || ny > o.home_y + ry)))
   {
      return Way::OutsideRange;
   }
   const map::Map& m = shown();
   const map::MapCell c = map::cell_at(m, nx, ny);
   if (map::cell_collision(c) != 0 || map::walls_block(map::behavior(map::cell_at(m, o.x, o.y)), map::behavior(c), d))
   {
      return Way::Blocked;
   }
   if (!map::may_enter(o.elevation, map::cell_elevation(c)))
   {
      return Way::Blocked;
   }
   const Walker& w = g.w;
   const bool walker_moving = w.motion != Motion::Idle && w.motion != Motion::Turning;
   if (((w.x == nx && w.y == ny) || (walker_moving && g.from_x == nx && g.from_y == ny)) &&
       levels_compatible(o.elevation, w.elevation))
   {
      return Way::Blocked;
   }
   for (uint8_t k = 0; k < g.object_count; ++k)
   {
      const Object& other = g_objects[k];
      if (k == i || !other.present || !levels_compatible(o.elevation, other.elevation))
      {
         continue;
      }
      if ((other.x == nx && other.y == ny) || (other.from_x == nx && other.from_y == ny))
      {
         return Way::Blocked;
      }
   }
   return Way::Free;
}

/// A random entry of an object's delay table (one draw).
/// @param o  the object
/// @return frames
uint16_t random_delay(const Object& o)
{
   const uint8_t table = static_cast<uint8_t>((o.movement_flags & map::kMoveDelaysMask) >> map::kMoveDelaysShift);
   const uint32_t r = rng::next16(rng::gameplay) % kDelayChoices;
   return table == map::kMoveDelaysShort ? kDelaysShort[r] : kDelaysMedium[r];
}

/// Faces an object's facing (a face action: its sprite stands).
/// @param i  local ID
void face_action(uint8_t i)
{
   Object& o = g_objects[i];
   o.move_dir = o.facing;
   if (o.sprite != nullptr && o.sprite->steps != nullptr && o.slot != kNoSlot)
   {
      step_face(o.slot, *o.sprite, anim_or_walk(*o.sprite, kWalkAnimFirst + o.facing, o.facing));
   }
}

/// Sets an object's facing and movement direction (direction fix keeps the facing).
/// @param o  the object
/// @param d  Direction
void set_direction(Object& o, uint8_t d)
{
   turn(o.facing, o.options, d);
   o.move_dir = d;
}

/// Starts a step of an object's machine: its first pixel moves this frame.
/// @param i  local ID
/// @param d  Direction
void machine_step(uint8_t i, uint8_t d)
{
   Object& o = g_objects[i];
   o.speed = route::kDefaultSpeed;   // walking: a pixel a frame
   o.in_place_pace = kNoPace;
   begin_step(o, d);
   advance_object(i);
}

/// Starts a walk in place of an object's machine (its first frame counts now).
/// @param i     local ID
/// @param pace  map::kInPlace...
void machine_in_place(uint8_t i, uint8_t pace)
{
   Object& o = g_objects[i];
   o.in_place = kInPlaceFrames[pace & kLegMask];
   o.in_place_pace = pace;
   if (o.sprite != nullptr && o.sprite->steps != nullptr && o.slot != kNoSlot)
   {
      step_begin(o.slot, *o.sprite, in_place_anim(*o.sprite, pace, o.facing));
      gfx::sprites::set_half_speed(o.slot, pace == map::kInPlaceSlow);
   }
   --o.in_place;
}

/// Counts a frame of a walk in place under way.
/// @param o  the object
/// @return true when it is over (the frame it ends)
bool in_place_done(Object& o)
{
   if (o.in_place == 0)
   {
      return true;
   }
   if (--o.in_place > 0)
   {
      return false;
   }
   if (o.slot != kNoSlot)
   {
      gfx::sprites::pause(o.slot, true);   // it stays on the entry it reached
   }
   return true;
}

/// Whether a spotter object watches the walker running in its sight box (the reference games' rule: the walker
/// runs, and its destination cell is at most `sight` cells away on both axes).
/// @param i  local ID
/// @return true when it does
bool watches_runner(uint8_t i)
{
   const Object& o = g_objects[i];
   const map::ObjectTemplate& t = tmpl(i);
   const Walker& w = g.w;
   if (t.sight == 0 || !w.running || w.motion != Motion::Walking)
   {
      return false;
   }
   const int dx = w.x - o.x;
   const int dy = w.y - o.y;
   return dx >= -t.sight && dx <= t.sight && dy >= -t.sight && dy <= t.sight;
}

/// The direction a watching object turns to: towards the walker on the longer axis (ties: the vertical one), then,
/// when its set lacks it, the other axis' direction, then its set's vertical direction.
/// @param i    local ID
/// @param set  the directions it may face (bit d = Direction d)
/// @return Direction
uint8_t watch_direction(uint8_t i, uint8_t set)
{
   const Object& o = g_objects[i];
   const int dx = g.w.x - o.x;
   const int dy = g.w.y - o.y;
   const int adx = dx < 0 ? -dx : dx;
   const int ady = dy < 0 ? -dy : dy;
   const uint8_t horizontal = static_cast<uint8_t>(dx < 0 ? DIR_LEFT : DIR_RIGHT);
   const uint8_t vertical = static_cast<uint8_t>(dy < 0 ? DIR_UP : DIR_DOWN);
   const uint8_t first = adx > ady ? horizontal : vertical;
   if ((set >> first & 1u) != 0)
   {
      return first;
   }
   const uint8_t other = adx > ady ? vertical : horizontal;
   if ((set >> other & 1u) != 0)
   {
      return other;
   }
   return (set >> DIR_UP & 1u) != 0 ? static_cast<uint8_t>(DIR_UP) : static_cast<uint8_t>(DIR_DOWN);
}

/// A Turn / Roam object's next direction: a random entry of its choice table (one draw).
/// @param o  the object
/// @return Direction
uint8_t random_choice(const Object& o)
{
   const uint32_t mask = (o.movement_flags & map::kMoveTwoChoices) != 0 ? kTwoChoicesMask : kFourChoicesMask;
   const auto k = static_cast<uint8_t>(rng::next16(rng::gameplay) & mask);
   return static_cast<uint8_t>((o.movement_arg >> (map::kCircuitLegShift * k)) & kLegMask);
}

/// The directions of a Turn object's choice table (bit d = Direction d).
/// @param o  the object
/// @return the set
uint8_t choice_set(const Object& o)
{
   const uint8_t n = (o.movement_flags & map::kMoveTwoChoices) != 0 ? kTwoChoices : kLegs;
   uint8_t set = 0;
   for (uint8_t k = 0; k < n; ++k)
   {
      set = static_cast<uint8_t>(set | 1u << ((o.movement_arg >> (map::kCircuitLegShift * k)) & kLegMask));
   }
   return set;
}

/// Whether an object stands on its spawn cell (C12: Object::home_x).
/// @param i  local ID
/// @return true when it does
bool at_home(uint8_t i)
{
   return g_objects[i].x == g_objects[i].home_x && g_objects[i].y == g_objects[i].home_y;
}

/// The direction a quarter-turn count clockwise from up (0 up, 1 right, 2 down, 3 left) and back.
constexpr uint8_t kAngleOf[NUM_DIRECTIONS] = {2, 0, 3, 1};   ///< By Direction: its clockwise angle from up.
constexpr uint8_t kDirectionOfAngle[NUM_DIRECTIONS] = {DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT};   ///< By angle.

/// A direction turned by quarter turns clockwise.
/// @param d      Direction
/// @param turns  quarter turns (any count; taken mod 4)
/// @return the direction
constexpr uint8_t turned(uint8_t d, int turns)
{
   return kDirectionOfAngle[(kAngleOf[d] + turns + NUM_DIRECTIONS * NUM_DIRECTIONS) % NUM_DIRECTIONS];
}
static_assert(turned(DIR_DOWN, 1) == DIR_LEFT && turned(DIR_UP, -1) == DIR_LEFT && turned(DIR_RIGHT, 2) == DIR_LEFT);

/// The leg of a circuit that leads back along an axis (the first leg opposite an earlier one), or kLegs.
/// @param legs  the circuit (2 bits a leg)
/// @return its index
uint8_t return_leg(uint8_t legs)
{
   for (uint8_t j = 1; j < kLegs; ++j)
   {
      const uint8_t d = (legs >> (map::kCircuitLegShift * j)) & kLegMask;
      for (uint8_t k = 0; k < j; ++k)
      {
         if (((legs >> (map::kCircuitLegShift * k)) & kLegMask) == opposite(d))
         {
            return j;
         }
      }
   }
   return kLegs;
}

/// A circuit's leg direction (a leg past the last wraps to the first).
/// @param o  the object
/// @return Direction
uint8_t leg_of(const Object& o)
{
   return static_cast<uint8_t>((o.movement_arg >> (map::kCircuitLegShift * (o.sequence & kLegMask))) & kLegMask);
}

/// Steps (or, blocked, walks in place) one leg of a Patrol or Circuit, after the range check that may move it on
/// to the next leg.
/// @param i     local ID
/// @param next  Patrol: turn back; Circuit: the next leg
void walk_leg(uint8_t i, bool next)
{
   Object& o = g_objects[i];
   Way w = way_for(i, o.move_dir);
   if (w == Way::OutsideRange)
   {
      ++o.sequence;
      set_direction(o, next ? leg_of(o) : opposite(o.move_dir));
      w = way_for(i, o.move_dir);
   }
   if (w == Way::Free)
   {
      machine_step(i, o.move_dir);
   }
   else
   {
      machine_in_place(i, map::kInPlaceWalk);
   }
   o.machine = kWalk;
}

/// A Mirror object's answer to the walker beginning a step or a turn: the same move, turned.
/// @param i  local ID
void mirror_walker(uint8_t i)
{
   Object& o = g_objects[i];
   const uint8_t ref = o.movement_arg & map::kMirrorFacingMask;
   const auto from = static_cast<uint8_t>(o.sequence - 1);   // the walker's facing at spawn
   const uint8_t d = turned(g.walker_began_dir, kAngleOf[ref] - kAngleOf[from]);
   set_direction(o, d);
   if (g.walker_began_walk && way_for(i, d) == Way::Free)
   {
      const uint8_t cls = static_cast<uint8_t>((o.movement_arg & map::kMirrorClassMask) >> map::kMirrorClassShift);
      const map::MapCell c = map::cell_at(shown(), o.x + dx_of(d), o.y + dy_of(d));
      if (cls == 0 || (map::behavior(c).passable >> (cls - 1) & 1u) != 0)
      {
         machine_step(i, d);
         return;
      }
   }
   face_action(i);
}

/// Runs an object's movement machine for this frame (the movements I3 adds).
/// @param i  local ID
void think_machine(uint8_t i)
{
   Object& o = g_objects[i];
   const bool watch = (o.movement_flags & map::kMoveWatchRunner) != 0;
   // At most a few steps run in one frame; the bound only guards against a data loop.
   for (int guard = 0; guard < kLegs * kLegs; ++guard)
   {
      switch (o.movement)
      {
      case map::Movement::Turn:
         if (o.machine == kStart)
         {
            face_action(i);
            o.delay = random_delay(o);
            o.machine = kWait;
            return;   // the wait starts next frame
         }
         if (o.machine == kWait)
         {
            if (--o.delay != 0 && !(watch && watches_runner(i)))
            {
               return;
            }
            o.machine = kChoose;
         }
         set_direction(o, watch && watches_runner(i) ? watch_direction(i, choice_set(o)) : random_choice(o));
         o.machine = kStart;
         continue;
      case map::Movement::Roam:
         if (o.machine == kStart)
         {
            face_action(i);
            o.delay = random_delay(o);
            o.machine = kWait;   // the wait counts this frame already
         }
         if (o.machine == kWait)
         {
            if (--o.delay != 0)
            {
               return;
            }
            o.machine = kChoose;
         }
         if (o.machine == kChoose)
         {
            const uint8_t d = random_choice(o);
            set_direction(o, d);
            if (way_for(i, d) != Way::Free)
            {
               o.machine = kStart;
               continue;
            }
            machine_step(i, d);
            o.machine = kWalk;
            return;
         }
         o.machine = kStart;   // kWalk: the step is over
         continue;
      case map::Movement::Rotate:
         if (o.machine == kStart)
         {
            face_action(i);
            o.delay = map::kRotateFrames;
            o.machine = kWait;
            return;
         }
         if (o.machine == kWait)
         {
            if (--o.delay == 0 || (watch && watches_runner(i)))
            {
               o.machine = kChoose;
            }
            return;   // the turn comes next frame
         }
         if (watch && watches_runner(i))
         {
            set_direction(o, watch_direction(i, (1u << NUM_DIRECTIONS) - 1));
         }
         else
         {
            set_direction(o, (o.movement_flags & map::kRotateClockwise) != 0 ? turn_right(o.facing)
                                                                              : turn_left(o.facing));
         }
         o.machine = kStart;
         continue;
      case map::Movement::Patrol:
         if (o.machine == kWalk)
         {
            if (o.in_place > 0 && !in_place_done(o))
            {
               return;
            }
            const bool was_in_place = o.in_place_pace != kNoPace;
            o.in_place_pace = kNoPace;
            o.machine = kStart;
            if (was_in_place)
            {
               return;   // a walk in place ends on its last frame; the next leg starts next frame
            }
         }
         set_direction(o, o.sequence != 0 ? opposite(o.base_facing) : o.base_facing);
         if (o.sequence != 0 && at_home(i))
         {
            o.sequence = 0;
            set_direction(o, opposite(o.move_dir));
         }
         walk_leg(i, false);
         return;
      case map::Movement::Circuit:
      {
         if (o.machine == kWalk)
         {
            if (o.in_place > 0 && !in_place_done(o))
            {
               return;
            }
            const bool was_in_place = o.in_place_pace != kNoPace;
            o.in_place_pace = kNoPace;
            o.machine = kStart;
            if (was_in_place)
            {
               return;
            }
         }
         const uint8_t back = return_leg(o.movement_arg);
         if (back < kLegs && o.sequence == back)
         {
            const uint8_t d = (o.movement_arg >> (map::kCircuitLegShift * back)) & kLegMask;
            const bool vertical = d == DIR_UP || d == DIR_DOWN;
            if (vertical ? o.y == o.home_y : o.x == o.home_x)
            {
               ++o.sequence;   // already back on that axis: skip the leg
            }
         }
         if (o.sequence == kLastLeg && at_home(i))
         {
            o.sequence = 0;
         }
         set_direction(o, leg_of(o));
         walk_leg(i, true);
         return;
      }
      case map::Movement::Mirror:
         if (o.sequence == 0)
         {
            o.sequence = static_cast<uint8_t>(g.w.facing + 1);
         }
         if (g.walker_began)
         {
            mirror_walker(i);
         }
         return;
      case map::Movement::InPlace:
         if (o.machine == kWalk)
         {
            if (!in_place_done(o))
            {
               return;
            }
            o.machine = kStart;
            return;   // the next walk in place starts next frame
         }
         machine_in_place(i, o.movement_arg);
         o.machine = kWalk;
         return;
      case map::Movement::Invisible:
         if (o.machine == kStart)
         {
            face_action(i);
            o.invisible = true;
            o.machine = kIdle;
         }
         return;
      default:
         return;
      }
   }
}
///@}

/// An object's own movement decision, when its timer runs out.
/// @param i  local ID
void think_object(uint8_t i)
{
   Object& o = g_objects[i];
   if (o.movement >= map::Movement::Turn)
   {
      think_machine(i);   // I3: the reference games' movement types
      return;
   }
   if (o.movement == map::Movement::Static || o.movement == map::Movement::Schedule ||
       o.movement == map::Movement::Custom)
   {
      return;   // release() turns Static ones back; Custom ones follow their route
   }
   if (o.timer > 0)
   {
      --o.timer;
      return;
   }
   switch (o.movement)
   {
   case map::Movement::LookAround:
      turn(o.facing, o.options, static_cast<uint8_t>(rng::below(rng::gameplay, NUM_DIRECTIONS)));
      o.timer = at_frequency(o, kLookFrames * (1 + rng::below(rng::gameplay, kLookPauseUnits)));
      break;
   case map::Movement::Wander:
   {
      const auto d = static_cast<uint8_t>(rng::below(rng::gameplay, NUM_DIRECTIONS));
      turn(o.facing, o.options, d);
      try_object_step(i, d, true);
      o.timer = at_frequency(o, kLookFrames * (1 + rng::below(rng::gameplay, kLookPauseUnits)));
      break;
   }
   case map::Movement::PaceVertical:
   case map::Movement::PaceHorizontal:
   {
      const bool vertical = o.movement == map::Movement::PaceVertical;
      uint8_t d = o.move_dir < NUM_DIRECTIONS ? o.move_dir : o.facing;
      if (vertical != (d == DIR_DOWN || d == DIR_UP))
      {
         d = vertical ? DIR_DOWN : DIR_RIGHT;
      }
      if (!try_object_step(i, d, true))
      {
         d = opposite(d);
         turn(o.facing, o.options, d);
         o.move_dir = d;
         try_object_step(i, d, true);
      }
      o.timer = at_frequency(o, kPaceFrames);
      break;
   }
   case map::Movement::Approach:
   {
      const uint8_t d = toward(o.x, o.y, g.w.x, g.w.y);
      if (d < NUM_DIRECTIONS && !try_object_step(i, d, false))
      {
         turn(o.facing, o.options, d);
      }
      o.timer = at_frequency(o, kPaceFrames);
      break;
   }
   default:
      break;
   }
}

/// Advances one moving object a frame.
/// @param i  local ID
void advance_object(uint8_t i)
{
   Object& o = g_objects[i];
   if (o.move_dir >= NUM_DIRECTIONS)
   {   // a jump: linear over the frames, an arc above
      const int total = o.jump_frames;
      const int done = total - o.remaining + 1;
      const int32_t fx = int32_t{o.from_x} * map::kMetatilePixels;
      const int32_t fy = int32_t{o.from_y} * map::kMetatilePixels;
      o.px = fx + (int32_t{o.x} * map::kMetatilePixels - fx) * done / total;
      o.py = fy + (int32_t{o.y} * map::kMetatilePixels - fy) * done / total;
      // Half a sine turn over the jump; longer jumps rise higher.
      const auto angle = static_cast<uint16_t>(done * kHalfTurn / total);
      o.lift = static_cast<uint8_t>((sin_q14(angle) * (kJumpHeight + total / kJumpHeightDivisor)) >> kQ14Shift);
      o.remaining = static_cast<uint8_t>(o.remaining - 1);
      if (o.remaining == 0)
      {
         o.moving = false;
         o.lift = 0;
         o.from_x = o.x;
         o.from_y = o.y;
         o.move_dir = o.facing;
         arrive_level(o);
         if (o.steps_left > 0)
         {   // a scripted walk asked for during the jump
            --o.steps_left;
            begin_step(o, o.next_dir);
         }
      }
      return;
   }
   int step = pixels_at(o.speed);
   if (step > o.remaining)
   {
      step = o.remaining;
   }
   o.px += dx_of(o.move_dir) * step;
   o.py += dy_of(o.move_dir) * step;
   o.remaining = static_cast<uint8_t>(o.remaining - step);
   if (o.remaining == 0)
   {
      o.moving = false;
      o.from_x = o.x;
      o.from_y = o.y;
      arrive_level(o);
      if (o.steps_left > 0)
      {   // a scripted walk through anything
         --o.steps_left;
         begin_step(o, o.next_dir);
      }
   }
}

/// Advances every object one frame.
void update_objects()
{
   for (uint8_t i = 0; i < g.object_count; ++i)
   {
      Object& o = g_objects[i];
      if (!o.active)
      {
         continue;
      }
      if (o.moving)
      {
         advance_object(i);
      }
      else if (g_routes[i].code != nullptr && (g_routes[i].script || (!g.locked && g.approach == kNoObject)))
      {
         run_route(i);
      }
      else if (!g.locked && g.approach == kNoObject)
      {
         think_object(i);
      }
   }
}

/// The first spotter that sees the standing walker (E-8): facing it in a
/// line within its sight, with every cell in between passable and free.
/// @return its local ID, or kNoObject
uint8_t spotter()
{
   const Walker& w = g.w;
   if (w.noclip || g.object_count == 0)
   {
      return kNoObject;
   }
   const map::Map& m = shown();
   for (uint8_t i = 0; i < g.object_count; ++i)
   {
      const Object& o = g_objects[i];
      const map::ObjectTemplate& t = m.objects[i];
      if (!o.active || o.moving || o.blind || t.sight == 0 || o.script == kNoScript)
      {
         continue;
      }
      if (t.until != map::kNoEventCondition && script::flag(script::FlagId{t.until}))
      {
         continue;
      }
      if (!map::levels_meet(o.elevation, w.elevation))
      {
         continue;
      }
      const int ddx = dx_of(o.facing);
      const int ddy = dy_of(o.facing);
      // The cheap test first: same row or column, ahead, within sight.
      const int dist = ddx != 0 ? (w.x - o.x) * ddx : (w.y - o.y) * ddy;
      if ((ddx != 0 ? w.y != o.y : w.x != o.x) || dist < 1 || dist > t.sight)
      {
         continue;
      }
      bool clear = true;
      for (int k = 1; k < dist && clear; ++k)
      {
         const int cx = o.x + ddx * k;
         const int cy = o.y + ddy * k;
         clear =
            can_enter(m, cx, cy, o.elevation, 0) == Blocked::None && object_on(cx, cy, o.elevation, i) == kNoObject;
      }
      if (clear)
      {
         return i;
      }
   }
   return kNoObject;
}

/// Starts a spotter's approach when one sees the walker.
/// @return true when one did (the walker waits)
bool check_sight()
{
   if (g.locked)
   {
      return false;
   }   // a script runs
   const uint8_t i = spotter();
   if (i == kNoObject)
   {
      return false;
   }
   g.approach = i;
   g.approach_timer = kSpotFrames;
   g.locked = true;
   return true;
}

/// Advances a spotter's approach; requests its script once it stands next to the walker.
void update_approach()
{
   if (g.approach == kNoObject)
   {
      return;
   }
   Object& o = g_objects[g.approach];
   if (g.approach_timer > 0)
   {
      if (--g.approach_timer == 0)
      {
         const int dist = (g.w.x - o.x) * dx_of(o.facing) + (g.w.y - o.y) * dy_of(o.facing);
         if (dist > 1)
         {
            move_object(g.approach, o.facing, static_cast<uint16_t>(dist - 1));
         }
      }
      return;
   }
   if (o.moving)
   {
      return;
   }
   turn(g.w.facing, g.w.options, opposite(o.facing));
   const uint8_t i = g.approach;
   g.approach = kNoObject;
   request(Request::Script, Source::Spotter, o.script, o.x, o.y);
   g.event.object = i;
}

/// The first Autorun object whose page is active.
/// @return true when one was requested
bool check_autorun()
{
   for (uint8_t i = 0; i < g.object_count; ++i)
   {
      const Object& o = g_objects[i];
      if (o.active && o.trigger == map::Trigger::Autorun && o.script != kNoScript)
      {
         request_object(i, Source::Autorun);
         return true;
      }
   }
   return false;
}

///@}

/// @name Routes (D-17)
///@{

/// Reads a little-endian 16-bit route argument.
/// @param p  its first byte
/// @return the value
uint16_t arg16(const uint8_t* p)
{
   return load_le16(p);
}

/// N2: starts a step of the camera mover (through anything; its first pixels move this frame, see update_camera()).
/// @param d  Direction
void camera_step(uint8_t d)
{
   g.cam_dir = d;
   g.cam_left = map::kMetatilePixels;
   g.cam_x = static_cast<int16_t>(g.cam_x + dx_of(d));
   g.cam_y = static_cast<int16_t>(g.cam_y + dy_of(d));
}

/// What a route's mover can do: an object, the walker or (N2) the camera mover.
struct Mover
{
   uint8_t slot;   ///< Object local ID, kWalkerRoute or kCameraRoute.

   /// Whether it is a map object.
   /// @return true for an object's slot
   bool object() const
   {
      return slot < kWalkerRoute;
   }
   /// Its cell.
   /// @param[out] x  column
   /// @param[out] y  row
   void cell(int& x, int& y) const
   {
      if (slot == kWalkerRoute)
      {
         x = g.w.x;
         y = g.w.y;
      }
      else if (slot == kCameraRoute)
      {
         x = g.cam_x;
         y = g.cam_y;
      }
      else
      {
         x = g_objects[slot].x;
         y = g_objects[slot].y;
      }
   }
   /// Its facing (writable).
   /// @return the facing
   uint8_t& facing() const
   {
      if (slot == kCameraRoute)
      {
         return g.cam_facing;
      }
      return slot == kWalkerRoute ? g.w.facing : g_objects[slot].facing;
   }
   /// Its options (writable).
   /// @return the options
   uint8_t& options() const
   {
      if (slot == kCameraRoute)
      {
         return g.cam_options;
      }
      return slot == kWalkerRoute ? g.w.options : g_objects[slot].options;
   }
   /// Turns (unless direction fix).
   /// @param d  Direction
   void face(uint8_t d) const
   {
      turn(facing(), options(), d);
   }
   /// Tries a step.
   /// @param d     Direction
   /// @param keep  keep the facing (backward steps)
   /// @return true when it moves
   bool step(uint8_t d, bool keep) const
   {
      const uint8_t before = facing();
      bool moved = false;
      if (slot == kCameraRoute)
      {
         camera_step(d);   // N2: the camera mover goes through anything
         face(d);
         moved = true;
      }
      else if (slot == kWalkerRoute)
      {
         const uint8_t speed = static_cast<uint8_t>(pixels_at(g.walker_speed != 0 ? g.walker_speed
                                                                                 : route::kDefaultSpeed));
         moved = try_step(d, speed == 0 ? uint8_t{1} : speed, Motion::Walking);
         if (moved)
         {
            g.routed = true;
            face(d);
         }
      }
      else
      {
         moved = try_object_step(slot, d, false);
      }
      if (keep)
      {
         facing() = before;
      }
      return moved;
   }
   /// Tries a jump.
   /// @param dx  cells right
   /// @param dy  cells down
   /// @return true when it jumps
   bool jump(int dx, int dy) const
   {
      if (slot == kCameraRoute)
      {
         return true;   // N2: the camera mover does not jump (the step is skipped)
      }
      if (slot != kWalkerRoute)
      {
         return try_object_jump(slot, dx, dy);
      }
      // The walker jumps along one axis (as a ledge jump), up to 8 cells.
      const int adx = dx < 0 ? -dx : dx;
      const int ady = dy < 0 ? -dy : dy;
      if (adx != 0 && ady != 0)
      {
         return false;
      }
      const int cells = adx + ady;
      if (cells == 0)
      {
         return true;
      }
      const uint8_t d = adx != 0 ? static_cast<uint8_t>(dx < 0 ? DIR_LEFT : DIR_RIGHT)
                                 : static_cast<uint8_t>(dy < 0 ? DIR_UP : DIR_DOWN);
      const int lx = g.w.x + dx;
      const int ly = g.w.y + dy;
      if ((g.w.options & route::kOptionThrough) == 0 &&
          (can_enter(shown(), lx, ly, g.w.elevation, g.w.vehicle_class) != Blocked::None ||
           object_on(lx, ly, g.w.elevation, kNoObject) != kNoObject))
      {
         return false;
      }
      face(d);
      start_move(d, cells, kRunSpeed, Motion::Jumping);
      g.routed = true;
      return true;
   }
   /// Whether it is still moving.
   /// @return true while it is
   bool busy() const
   {
      if (slot == kCameraRoute)
      {
         return g.cam_left > 0;
      }
      if (slot == kWalkerRoute)
      {
         return g.w.motion != Motion::Idle && g.w.motion != Motion::Turning;
      }
      return g_objects[slot].moving;
   }
};

/// N2: starts the walk-in-place animation of a route's InPlace step (sprites with a step table; I4 only waited).
/// @param slot  object local ID, kWalkerRoute or kCameraRoute
/// @param pace  map::kInPlace...
/// @return true when an animation started (end_in_place() stops it)
bool start_in_place(uint8_t slot, uint8_t pace)
{
   if (slot == kWalkerRoute)
   {
      if (g.slot == kNoSlot || g.sprite == nullptr || g.sprite->steps == nullptr)
      {
         return false;
      }
      g.walker_in_place = true;
      step_begin(g.slot, *g.sprite, in_place_anim(*g.sprite, pace, g.w.facing));
      gfx::sprites::set_half_speed(g.slot, pace == map::kInPlaceSlow);
      return true;
   }
   if (slot >= kWalkerRoute)
   {
      return false;   // the camera mover has no sprite
   }
   Object& o = g_objects[slot];
   if (o.sprite == nullptr || o.sprite->steps == nullptr || o.slot == kNoSlot)
   {
      return false;
   }
   o.in_place_pace = pace;   // draw_step_object() leaves the animation running
   step_begin(o.slot, *o.sprite, in_place_anim(*o.sprite, pace, o.facing));
   gfx::sprites::set_half_speed(o.slot, pace == map::kInPlaceSlow);
   return true;
}

/// N2: ends a route's walk in place: the sprite stands on the entry it reached (as a machine's walk in place ends).
/// @param slot  object local ID or kWalkerRoute
void end_in_place(uint8_t slot)
{
   if (slot == kWalkerRoute)
   {
      g.walker_in_place = false;
      if (g.slot != kNoSlot)
      {
         gfx::sprites::pause(g.slot, true);
         gfx::sprites::set_half_speed(g.slot, false);
      }
      return;
   }
   if (slot >= kWalkerRoute)
   {
      return;
   }
   Object& o = g_objects[slot];
   o.in_place_pace = kNoPace;
   if (o.slot != kNoSlot)
   {
      gfx::sprites::pause(o.slot, true);
      gfx::sprites::set_half_speed(o.slot, false);
   }
}

/// Runs a route slot's steps for this frame (when its mover stands).
/// @param slot  object local ID, kWalkerRoute or (N2) kCameraRoute
void run_route(uint8_t slot)
{
   RouteRun& r = g_routes[slot];
   const Mover m{slot};
   if (r.code == nullptr || m.busy())
   {
      return;
   }
   if (r.wait > 0)
   {
      --r.wait;
      return;
   }
   if (r.in_place)
   {
      end_in_place(slot);   // N2: the walk in place is over: the sprite stands on the entry it reached
      r.in_place = false;
   }
   const uint8_t flags = r.code[0];
   for (int n = 0; n < kRouteOpsPerFrame && r.code != nullptr; ++n)
   {
      const uint8_t* p = r.code + r.pc;
      const auto op = static_cast<RouteOp>(*p);
      int x = 0;
      int y = 0;
      m.cell(x, y);
      uint8_t d = NUM_DIRECTIONS;
      uint16_t size = 1;
      bool move = false;
      switch (op)
      {
      case RouteOp::End:
         if ((flags & route::kRouteRepeat) != 0 && n > 0)
         {
            r.pc = 1;
            continue;
         }
         if ((flags & route::kRouteRepeat) != 0)
         {   // nothing ran: wait a frame
            r.pc = 1;
            return;
         }
         r.code = nullptr;
         if (r.script)
         {   // a script's route is over: the object's own movement takes over again
            r.script = false;
            if (m.object() && g_objects[slot].movement == map::Movement::Custom)
            {
               start_route(slot, map_route(g_objects[slot].route), false);
            }
         }
         return;
      case RouteOp::Move:
         if (r.left == 0)
         {
            r.left = p[kSecondArg] == 0 ? uint8_t{1} : p[kSecondArg];
         }
         d = p[1] & kDirectionMask;
         if (m.step(d, false))
         {
            if (--r.left == 0)
            {
               r.pc = static_cast<uint16_t>(r.pc + kStepTwoBytes);
            }
            return;
         }
         m.face(d);
         if ((flags & route::kRouteSkip) != 0)
         {
            r.left = 0;
            r.pc = static_cast<uint16_t>(r.pc + kStepTwoBytes);
            continue;
         }
         return;   // blocked: try again next frame
      case RouteOp::Random:
         d = static_cast<uint8_t>(rng::below(rng::gameplay, NUM_DIRECTIONS));
         move = true;
         break;
      case RouteOp::Toward:
      case RouteOp::Away:
         if (slot == kWalkerRoute || slot == kCameraRoute)
         {   // the walker has no walker to go to (nor the camera mover: N2)
            r.pc = static_cast<uint16_t>(r.pc + 1);
            continue;
         }
         d = toward(x, y, g.w.x, g.w.y);
         if (d < NUM_DIRECTIONS && op == RouteOp::Away)
         {
            d = opposite(d);
         }
         move = d < NUM_DIRECTIONS;
         if (!move)
         {
            r.pc = static_cast<uint16_t>(r.pc + 1);
            continue;
         }
         break;
      case RouteOp::Forward:
         d = m.facing();
         move = true;
         break;
      case RouteOp::Backward:
         d = opposite(m.facing());
         move = true;
         break;
      case RouteOp::Jump:
         if (m.jump(static_cast<int8_t>(p[1]), static_cast<int8_t>(p[kSecondArg])))
         {
            r.pc = static_cast<uint16_t>(r.pc + kStepTwoBytes);
            return;
         }
         if ((flags & route::kRouteSkip) != 0)
         {
            r.pc = static_cast<uint16_t>(r.pc + kStepTwoBytes);
            continue;
         }
         return;
      case RouteOp::Wait:
         r.wait = static_cast<uint16_t>(arg16(p + 1) - 1);
         r.pc = static_cast<uint16_t>(r.pc + kStepTwoBytes);
         return;
      case RouteOp::Face:
         m.face(p[1] & kDirectionMask);
         size = kStepOneByte;
         break;
      case RouteOp::TurnRight:
         m.face(turn_right(m.facing()));
         break;
      case RouteOp::TurnLeft:
         m.face(turn_left(m.facing()));
         break;
      case RouteOp::TurnAround:
         m.face(opposite(m.facing()));
         break;
      case RouteOp::TurnRandom:
         m.face(static_cast<uint8_t>(rng::below(rng::gameplay, NUM_DIRECTIONS)));
         break;
      case RouteOp::FaceToward:
      case RouteOp::FaceAway:
         if (m.object())
         {
            d = toward(x, y, g.w.x, g.w.y);
            if (d < NUM_DIRECTIONS)
            {
               m.face(op == RouteOp::FaceAway ? opposite(d) : d);
            }
         }
         break;
      case RouteOp::Flag:
         script::set_flag(script::FlagId{arg16(p + 1)}, p[kFlagOnArg] != 0);
         size = kStepFlag;
         break;
      case RouteOp::Speed:
         if (m.object())
         {
            g_objects[slot].speed = p[1] >= 1 && p[1] <= route::kMaxSpeed ? p[1] : route::kDefaultSpeed;
         }
         else if (slot == kCameraRoute)
         {
            g.cam_speed = p[1] >= 1 && p[1] <= route::kMaxSpeed ? p[1] : route::kDefaultSpeed;
         }
         else
         {   // N2: the walker's route steps (a fast walk: MOVEMENT_ACTION_WALK_FAST_*)
            g.walker_speed = p[1] >= 1 && p[1] <= route::kMaxSpeed ? p[1] : route::kDefaultSpeed;
         }
         size = kStepOneByte;
         break;
      case RouteOp::Frequency:
         if (m.object())
         {
            g_objects[slot].frequency = p[1] >= 1 && p[1] <= route::kMaxFrequency ? p[1] : route::kDefaultFrequency;
         }
         size = kStepOneByte;
         break;
      case RouteOp::Option:
         m.options() = static_cast<uint8_t>(p[kSecondArg] != 0 ? (m.options() | p[1]) : (m.options() & ~p[1]));
         size = kStepTwoBytes;
         break;
      case RouteOp::Sprite:
         if (m.object())
         {
            set_sprite(slot, AssetKey{load_le32(p + 1)});
         }
         else if (slot == kWalkerRoute)
         {
            const asset::Sprite* s = asset::sprite(AssetKey{load_le32(p + 1)});
            if (s != nullptr && g.slot != kNoSlot)
            {
               gfx::sprites::load(g.slot, *s);
               g.sprite = s;
               g.anim = kNoAnim;
            }
         }
         size = kStepSprite;
         break;
      case RouteOp::Visible:
         if (slot == kWalkerRoute)
         {
            g.w.visible = p[1] != 0;
         }
         else if (m.object())
         {
            g_objects[slot].visible = p[1] != 0;
         }
         size = kStepOneByte;
         break;
      case RouteOp::InPlace:
      {
         // I4: faces the way and stands for one step's time at that speed (16 pixels at its pixels per frame)
         m.face(p[1] & kDirectionMask);
         const uint8_t level = p[kSecondArg] <= route::kMaxSpeed ? p[kSecondArg] : route::kDefaultSpeed;
         r.wait = static_cast<uint16_t>(kStepFramesAt[level] - 1);
         r.pc = static_cast<uint16_t>(r.pc + kStepTwoBytes);
         r.in_place = start_in_place(slot, kInPlacePaceAt[level]);   // N2: its walk in place animates
         return;
      }
      case RouteOp::FaceOriginal:
         if (m.object())
         {
            m.face(g_objects[slot].base_facing);
         }
         break;
      case RouteOp::Game:
         // I4: the game's step runs until it is done
         if (!game::route_step(m.object() ? slot : (slot == kCameraRoute ? kCameraObject : kNoObject), p[1],
                               p[kSecondArg]))
         {
            return;
         }
         size = kStepTwoBytes;
         break;
      case RouteOp::NUM_ROUTE_OPS:
         ENGINE_ASSERT(false && "bad route step");
         r.code = nullptr;
         return;
      }
      if (move)
      {
         if (m.step(d, op == RouteOp::Backward))
         {
            r.pc = static_cast<uint16_t>(r.pc + 1);
            return;
         }
         m.face(op == RouteOp::Backward ? m.facing() : d);
         if ((flags & route::kRouteSkip) != 0)
         {
            r.pc = static_cast<uint16_t>(r.pc + 1);
            continue;
         }
         return;
      }
      r.pc = static_cast<uint16_t>(r.pc + size);
   }
}

///@}

/// Requests a warp event's landing.
/// @param m  the map
/// @param e  the warp event
/// @param x  its cell column
/// @param y  its cell row
void request_warp(const map::Map& m, const map::Event& e, int x, int y)
{
   ENGINE_ASSERT(m.warps != nullptr && e.target < m.warp_count);
   const map::Warp& w = m.warps[e.target];
   request(Request::Warp, Source::Warp, kNoScript, x, y);
   g.event.map = w.map;
   g.event.x = w.x;
   g.event.y = w.y;
   g.event.transition = w.transition;
}

/// Checks the events of the cell the walker arrived on (B-3), in the reference games'
/// order: trigger, step warp, step hooks.
/// @param m  the map
/// @return true when one fired (the walker stops)
bool arrival_events(const map::Map& m)
{
   const Walker& w = g.w;
   for (const map::Event* t = map::find_event(m, w.x, w.y, map::EventKind::Trigger, w.elevation); t != nullptr;
        t = map::find_event(m, w.x, w.y, map::EventKind::Trigger, w.elevation, t))
   {
      const bool holds = t->condition == map::kNoEventCondition ||
                         script::var(script::VarId{t->condition}) == t->value;
      if (holds && (t->direction & map::kTriggerImmediate) != 0)
      {
         // C10: runs to its end now; the walker keeps moving and the next events of the cell are checked
         script::run_now(script::ScriptId{t->target});
         continue;
      }
      if (holds)
      {
         request(Request::Script, Source::Trigger, t->target, w.x, w.y);
         return true;
      }
   }
   const map::Event* wp = map::find_event(m, w.x, w.y, map::EventKind::Warp, w.elevation);
   if (wp != nullptr && wp->direction == map::kAnyDirection)
   {
      request_warp(m, *wp, w.x, w.y);
      return true;
   }
   const map::World& world = map::kWorld;
   uint16_t asked = kNoScript;
   for (uint8_t i = 0; i < world.step_hook_count; ++i)
   {
      const map::StepHook& h = world.step_hooks[i];
      if ((w.steps & h.period_mask) != h.phase)
      {
         continue;
      }
      const uint16_t s = game::step_hook(i);
      if (asked == kNoScript)
      {
         asked = s;
      }
   }
   if (asked != kNoScript)
   {
      request(Request::Script, Source::StepHook, asked, w.x, w.y);
      return true;
   }
   return false;
}

/// Handles arriving on a cell after a scripted or routed step: elevation, the next step.
void arrive_scripted()
{
   Walker& w = g.w;
   w.elevation = map::arrival_level(w.elevation, map::cell_elevation(map::cell_at(shown(), w.x, w.y)));
   w.motion = Motion::Idle;
   w.lift = 0;
   g.from_x = w.x;
   g.from_y = w.y;
   g.routed = false;
   if (!g.scripted)
   {
      return;
   }   // a route step: the route goes on
   if (g.walker_steps > 0)
   {
      --g.walker_steps;
      turn(w.facing, w.options, g.walker_dir);
      start_move(g.walker_dir, 1, kWalkSpeed, Motion::Walking);
   }
   else
   {
      g.scripted = false;
   }
}

/// Handles arriving on a cell: counters, elevation, events, encounters, then Slide / Push.
void arrive()
{
   Walker& w = g.w;
   if (g.scripted || g.routed)
   {
      arrive_scripted();
      return;
   }
   const map::Map& m = shown();
   const map::MapCell c = map::cell_at(m, w.x, w.y);
   ++w.steps;
   if (map::cell_encounter(c))
   {
      ++w.encounter_steps;
   }
   // The walker takes the cell's level (by default 0 included: stairs meet any level, so it
   // can leave them onto either side), unless the level keeps the mover's (bridges).
   w.elevation = map::arrival_level(w.elevation, map::cell_elevation(c));
   w.motion = Motion::Idle;
   w.lift = 0;
   g.from_x = w.x;
   g.from_y = w.y;
   if (w.noclip)
   {
      return;
   }
   // A spotter that sees the walker comes first (the reference games check spotters before step events).
   if (check_sight())
   {
      return;
   }
   // A Through object with a touch trigger on this cell (D-17).
   const uint8_t here = object_on(w.x, w.y, w.elevation, kNoObject, true);
   if (here != kNoObject && touch_trigger(g_objects[here]) && g_objects[here].script != kNoScript && quiet())
   {
      request_object(here, Source::Touch);
      return;
   }
   // A map event stops the walker here (no slide or push); the game takes it.
   if (arrival_events(m))
   {
      return;
   }
   // An encounter stops the walker here (no slide or push); the game takes it.
   if (g.encounters && !g.has_encounter && encounter::roll_step(m, c, w.vehicle_class, rng::gameplay, &g.encounter))
   {
      g.has_encounter = true;
      return;
   }
   const map::Behavior& b = map::behavior(c);
   if (b.kind == map::BehaviorKind::Slide)
   {
      try_step(w.move_dir, kRunSpeed, Motion::Sliding);
   }
   else if (b.kind == map::BehaviorKind::Push)
   {
      turn(w.facing, w.options, b.direction);
      try_step(b.direction, kRunSpeed, Motion::Pushed);
   }
}

/// F2: asks the attached field actions' dispatcher about a faced target and requests the script it gives.
/// @param target  Behavior or Object
/// @param value   the behavior index or the object's kind
/// @param object  the object's local ID, or kNoObject (it becomes the script's self)
/// @param x       the faced cell's column
/// @param y       its row
/// @return true when a script was requested
bool offer_action(script::ActionTarget target, uint8_t value, uint8_t object, int x, int y)
{
   if (g_action_rules == nullptr)
   {
      return false;
   }
   const ActionFace face{target, value, object, g.w.facing, static_cast<int16_t>(x), static_cast<int16_t>(y)};
   const uint16_t s = g_action_rules(face);
   if (s == kNoScript)
   {
      return false;
   }
   request(Request::Script, Source::Action, s, x, y);
   g.event.object = object;
   return true;
}

/// Whether pressing A on an object starts its script.
/// @param o  the object
/// @return true for Action and the touch triggers (RPG Maker XP's)
bool talks(const Object& o)
{
   return !o.moving && o.script != kNoScript && (o.trigger == map::Trigger::Action || touch_trigger(o));
}

/// Starts a turn in place (or, a bump, a step's walk in place) of the walker.
/// @param d     its new facing
/// @param bump  a bump (a blocked step of a class that bumps: one step's time, walking) rather than a turn
void start_turn(uint8_t d, bool bump)
{
   Walker& w = g.w;
   const map::VehicleClass& v = map::vehicle_class(w.vehicle_class);
   turn(w.facing, w.options, d);
   w.motion = Motion::Turning;
   // I3: the class's turn time; a bump lasts a walking step (a cell at its walking speed)
   const uint8_t turn_frames = v.turn_frames != 0 ? v.turn_frames : kTurnFrames;
   w.timer = bump ? static_cast<uint8_t>(map::kMetatilePixels / (v.walk_speed != 0 ? v.walk_speed : 1)) : turn_frames;
   g.bumping = bump;
   began(false, d);
   walker_step_anim(bump ? kWalkAnimFirst : kTurnAnimFirst);
}

/// The press, arrow or door warp the walker's input fires (B-3, I3), or nullptr.
/// @param m       the map
/// @param intent  input (a direction is held)
/// @param[out] at_x  the warp's cell
/// @param[out] at_y  its row
/// @return the warp event
const map::Event* pressed_warp(const map::Map& m, const Intent& intent, int& at_x, int& at_y)
{
   const Walker& w = g.w;
   // a press (or arrow) warp under the walker in the pressed direction; an arrow warp needs the walker's facing
   for (const map::Event* e = map::find_event(m, w.x, w.y, map::EventKind::Warp, w.elevation); e != nullptr;
        e = map::find_event(m, w.x, w.y, map::EventKind::Warp, w.elevation, e))
   {
      const uint8_t d = e->direction & map::kEventDirectionMask;
      if (d == intent.direction && (e->direction & map::kWarpFromAdjacent) == 0 &&
          ((e->direction & map::kWarpNeedsFacing) == 0 || w.facing == d))
      {
         at_x = w.x;
         at_y = w.y;
         return e;
      }
   }
   // I3: a door in front, facing it and pressing towards it
   if (intent.direction != w.facing)
   {
      return nullptr;
   }
   const int fx = w.x + dx_of(w.facing);
   const int fy = w.y + dy_of(w.facing);
   for (const map::Event* e = map::find_event(m, fx, fy, map::EventKind::Warp, w.elevation); e != nullptr;
        e = map::find_event(m, fx, fy, map::EventKind::Warp, w.elevation, e))
   {
      if ((e->direction & map::kWarpFromAdjacent) != 0 && (e->direction & map::kEventDirectionMask) == w.facing)
      {
         at_x = fx;
         at_y = fy;
         return e;
      }
   }
   return nullptr;
}

/// C13b: the level the walker presses A with (the faced position's): its cell's level when that level meets every
/// level and does not keep the mover's (a stairs cell: the reference games' faced position takes it), else its own.
/// @param m  the shown map
/// @return the level
uint8_t faced_level(const map::Map& m)
{
   const Walker& w = g.w;
   const uint8_t cell = map::cell_elevation(map::cell_at(m, w.x, w.y));
   const map::Elevation& e = map::elevation(cell);
   return e.meets == map::kMeetsAll && (e.flags & map::kElevationKeep) == 0 ? cell : w.elevation;
}

/// C13b: requests the script of the world's first cell interaction rule (map::Interaction) that holds for the faced
/// cell.
/// @param m      the shown map
/// @param x      the faced cell's column
/// @param y      its row
/// @param level  the faced position's level (faced_level())
/// @param after  the rules kept for after the field actions (kInteractAfterActions), else the others
/// @return true when a script was requested
bool interact_cell(const map::Map& m, int x, int y, uint8_t level, bool after)
{
   const map::World& world = map::kWorld;
   if (world.interactions == nullptr)
   {
      return false;
   }
   const Walker& w = g.w;
   const map::MapCell c = map::cell_at(m, x, y);
   const uint8_t behavior = map::cell_behavior(c);
   for (uint16_t i = 0; i < world.interaction_count; ++i)
   {
      const map::Interaction& r = world.interactions[i];
      const bool kept = (r.flags & map::kInteractAfterActions) != 0;
      if (r.behavior != behavior || kept != after)
      {
         continue;
      }
      // its conditions: the facing, the vehicle class, the faced level against the cell's, a set flag
      const bool facing = r.facing == map::kAnyDirection || r.facing == w.facing;
      const bool vehicle = r.vehicle == map::kAnyVehicle || r.vehicle == w.vehicle_class;
      const bool same = (r.flags & map::kInteractSameLevel) == 0 || level == map::cell_elevation(c);
      const bool flag = r.flag == map::kNoEventCondition || script::flag(script::FlagId{r.flag});
      if (facing && vehicle && same && flag)
      {
         request(Request::Script, Source::Interaction, r.script, x, y);
         return true;
      }
   }
   return false;
}

/// Idle / turning logic for one frame.
/// @param intent      input
/// @param continuing  the walker was moving last frame (it just arrived): a class that keeps moving steps into
///                    another held direction without turning
void think(const Intent& intent, bool continuing = false)
{
   Walker& w = g.w;
   const map::VehicleClass& v = map::vehicle_class(w.vehicle_class);
   const bool keeps_moving = (v.flags & map::kVehicleKeepsMoving) != 0;
   if (w.motion == Motion::Turning)
   {
      if (w.timer > 0)
      {
         --w.timer;
      }
      if (w.timer > 0)
      {
         return;
      }
      w.motion = Motion::Idle;
      continuing = g.bumping && keeps_moving;   // a bump counts as moving; a turn does not
      g.bumping = false;
   }
   if (g.has_encounter || g.event.what != Request::None || g.approach != kNoObject)
   {
      return;
   }
   if (check_sight())
   {
      return;
   }   // a look-around spotter may turn to the walker
   if (!g.locked && check_autorun())
   {
      return;
   }
   const map::Map& m = shown();
   if (intent.interact && !w.noclip)
   {
      const int fx = w.x + dx_of(w.facing);
      const int fy = w.y + dy_of(w.facing);
      const uint8_t level = faced_level(m);   // C13b: the faced position's level (a stairs cell's own)
      // An object on the faced cell (B-21), before signs; then a Through one underfoot (D-17).
      uint8_t oi = object_on(fx, fy, level, kNoObject, true);
      if (oi == kNoObject && map::behavior(map::cell_at(m, fx, fy)).kind == map::BehaviorKind::Counter)
      {   // I3: across a counter, the object on the cell beyond
         oi = object_on(fx + dx_of(w.facing), fy + dy_of(w.facing), level, kNoObject, true);
      }
      if (oi == kNoObject)
      {
         const uint8_t under = object_on(w.x, w.y, w.elevation, kNoObject, true);
         if (under != kNoObject && (g_objects[under].options & route::kOptionThrough) != 0 && talks(g_objects[under]))
         {
            oi = under;
         }
      }
      if (oi != kNoObject)
      {
         Object& o = g_objects[oi];
         if (talks(o))
         {
            if (o.x != w.x || o.y != w.y)
            {
               turn(o.facing, o.options, opposite(w.facing));
            }
            request_object(oi, Source::Object);
            return;
         }
         if (tmpl(oi).kind != map::kNoObjectKind && !o.moving &&
             offer_action(script::ActionTarget::Object, tmpl(oi).kind, oi, o.x, o.y))   // F2: a bush, a boulder
         {
            return;
         }
         // C13b: an object that starts nothing leaves the press to the cell's sign and interactions (the reference
         // games' object without a script)
      }
      // A sign on the faced cell (B-3).
      for (const map::Event* s = map::find_event(m, fx, fy, map::EventKind::Sign, level); s != nullptr;
           s = map::find_event(m, fx, fy, map::EventKind::Sign, level, s))
      {
         if ((s->direction == map::kAnyDirection || s->direction == w.facing) &&
             (s->condition == map::kNoEventCondition || !script::flag(script::FlagId{s->condition})))
         {
            request(Request::Script, Source::Sign, s->target, fx, fy);
            return;
         }
      }
      // C13b: the world's cell interactions of the faced behavior (a PC, a bookshelf)
      if (interact_cell(m, fx, fy, level, false))
      {
         return;
      }
      // F2: a cell the walker cannot enter as it is (water on foot; C13b: or on another level, as water below a
      // bank is): the field actions on its behavior
      const Blocked faced = can_enter(m, fx, fy, w.elevation, w.vehicle_class);
      if ((faced == Blocked::Terrain || faced == Blocked::Elevation) &&
          offer_action(script::ActionTarget::Behavior, map::cell_behavior(map::cell_at(m, fx, fy)), kNoObject, fx,
                       fy))
      {
         return;
      }
      // C13b: then the interactions kept for after the field actions (a fallback message)
      if (interact_cell(m, fx, fy, level, true))
      {
         return;
      }
   }
   if (intent.direction >= NUM_DIRECTIONS)
   {
      return;
   }
   if (!w.noclip)
   {
      int at_x = w.x;
      int at_y = w.y;
      const map::Event* pw = pressed_warp(m, intent, at_x, at_y);
      if (pw != nullptr)
      {
         turn(w.facing, w.options, intent.direction);
         request_warp(m, *pw, at_x, at_y);
         return;
      }
   }
   if (intent.direction != w.facing && (w.options & route::kOptionDirFix) == 0 && !(continuing && keeps_moving))
   {
      start_turn(intent.direction, false);
      return;
   }
   // The vehicle class sets the speeds; a run where running is not allowed walks.
   w.running = intent.run && can_run();
   turn(w.facing, w.options, intent.direction);   // I3: a class that keeps moving may change direction mid-walk
   if (try_step(intent.direction, w.running ? v.run_speed : v.walk_speed, Motion::Walking, true))
   {
      began(true, intent.direction);
   }
   else if ((v.flags & map::kVehicleBumps) != 0 && w.last_blocked != Blocked::None &&
            w.last_blocked != Blocked::Rule && g.event.what == Request::None)
   {
      w.running = false;
      start_turn(intent.direction, true);   // I3: a bump walks in place
   }
}

/// Centres the camera on the walker (N2: or the camera mover; plus the offset a removed mover left) plus the camera
/// offset.
void follow()
{
   const Walker& w = g.w;
   const int32_t px = (g.camera_on ? g.cam_px : w.px) + g.cam_rest_dx;
   const int32_t py = (g.camera_on ? g.cam_py : w.py) + g.cam_rest_dy;
   map::set_camera(px + kCellCentre - hal::video::kScreenWidth / kHalf + g.cam_dx,
                   py + kCellCentre - hal::video::kScreenHeight / kHalf + g.cam_dy);
}

/// N2: one frame of the camera mover: its route (while it stands), then its step's pixels at its speed.
void update_camera()
{
   if (!g.camera_on)
   {
      return;
   }
   if (g.cam_left == 0)
   {
      run_route(kCameraRoute);
   }
   if (g.cam_left == 0)
   {
      return;
   }
   int step = pixels_at(g.cam_speed);
   if (step > g.cam_left)
   {
      step = g.cam_left;
   }
   g.cam_px += dx_of(g.cam_dir) * step;
   g.cam_py += dy_of(g.cam_dir) * step;
   g.cam_left = static_cast<uint8_t>(g.cam_left - step);
}

/// Positions and animates the walker's sprite.
void draw_sprite()
{
   if (g.slot == kNoSlot || g.sprite == nullptr)
   {
      return;
   }
   const Walker& w = g.w;
   gfx::sprites::show(g.slot, w.visible);
   const int sw = hal::video::obj_width(g.sprite->shape, g.sprite->size);
   const int sh = hal::video::obj_height(g.sprite->shape, g.sprite->size);
   const int32_t sx = w.px + kCellCentre - sw / kHalf - map::camera_x() + g.walker_dx;   // N2: its sprite offset
   const int32_t sy = w.py + map::kMetatilePixels - sh - w.lift - map::camera_y() + g.walker_dy;
   gfx::sprites::set_position(g.slot, static_cast<int16_t>(sx), static_cast<int16_t>(sy));
   const uint8_t pr = (w.options & route::kOptionOnTop) != 0 ? uint8_t{0} : map::elevation(w.elevation).priority;
   gfx::sprites::set_priority(g.slot, pr);
   // DS1: as the objects (N2: or the sub-priority a script fixed)
   gfx::sprites::set_order(g.slot, g.walker_fixed ? g.walker_order
                                                  : map::sub_priority(sy + sh + w.lift - g.walker_dy, w.elevation));
   const bool moving = w.motion != Motion::Idle && w.motion != Motion::Turning;
   // The vehicle class's set starts at its first animation (on foot: 0).
   const int base = map::vehicle_class(w.vehicle_class).first_animation;
   if (g.sprite->steps != nullptr)
   {   // I3: step animations: steps and turns set theirs when they start; standing, the walk set paused
      if (w.motion == Motion::Idle && !g.walker_in_place)   // N2: a route's walk in place keeps its animation
      {
         step_face(g.slot, *g.sprite, anim_or_walk(*g.sprite, base + kWalkAnimFirst + w.facing, w.facing));
      }
      return;
   }
   const int count = g.sprite->animation_count;
   int anim = base + w.facing;
   if ((moving && (w.options & route::kOptionWalkAnim) != 0) || (w.options & route::kOptionStepAnim) != 0)
   {
      const bool run_anims = count >= base + kAnimsWithRun;
      anim = base + (w.running && w.motion == Motion::Walking && run_anims ? kRunAnimFirst : kWalkAnimFirst) +
             w.facing;
   }
   if (anim >= count)
   {   // fall back to the class's standing set, then to the first one
      anim = base + w.facing < count ? base + w.facing : (w.facing < count ? w.facing : 0);
   }
   if (anim != g.anim)
   {
      g.anim = static_cast<uint8_t>(anim);
      gfx::sprites::play(g.slot, g.anim);
   }
}

}   // namespace

Blocked check_step(int x, int y, uint8_t d, uint8_t elevation, uint8_t vehicle, bool* jump)
{
   const map::Map& m = shown();
   *jump = false;
   const int nx = x + dx_of(d);
   const int ny = y + dy_of(d);
   const map::MapCell target = map::cell_at(m, nx, ny);
   const map::Behavior& tb = map::behavior(target);
   if (map::walls_block(map::behavior(map::cell_at(m, x, y)), tb, d))
   {   // I2: a wall on the side left or entered (one-way fences)
      return Blocked::Collision;
   }
   if (tb.kind == map::BehaviorKind::Ledge)
   {
      // N2: the ledge's behaviour wins over the cell's collision in its direction (the reference games test the
      // ledge before the collision: their ledge cells keep a collision bit, which blocks the other ways only)
      if (tb.direction != d)
      {
         return Blocked::Ledge;
      }
      const int lx = nx + dx_of(d);
      const int ly = ny + dy_of(d);
      const map::MapCell landing = map::cell_at(m, lx, ly);
      if (map::behavior(landing).kind == map::BehaviorKind::Ledge)
      {
         return Blocked::Ledge;
      }
      const Blocked b = can_enter(m, lx, ly, elevation, vehicle);
      if (b != Blocked::None)
      {
         return Blocked::Ledge;
      }
      *jump = true;
      return Blocked::None;
   }
   return can_enter(m, nx, ny, elevation, vehicle);
}

void place(int x, int y, uint8_t facing)
{
   const map::Map& m = shown();
   const Walker old = g.w;
   g.w = fresh_walker();
   g.w.noclip = old.noclip;
   g.w.vehicle_class = old.vehicle_class;
   g.w.options = old.options;
   g.w.visible = old.visible;
   g.has_encounter = false;
   g.event = FieldEvent{};
   g.scripted = false;
   g.routed = false;
   g.walker_steps = 0;
   g_routes[kWalkerRoute] = RouteRun{};
   if (g.approach != kNoObject)
   {
      g.locked = false;
   }   // the approach (and its lock) ends
   g.approach = kNoObject;
   g.w.x = static_cast<int16_t>(x);
   g.w.y = static_cast<int16_t>(y);
   g.from_x = g.w.x;
   g.from_y = g.w.y;
   g.w.px = static_cast<int32_t>(x) * map::kMetatilePixels;
   g.w.py = static_cast<int32_t>(y) * map::kMetatilePixels;
   g.w.facing = facing < NUM_DIRECTIONS ? facing : static_cast<uint8_t>(DIR_DOWN);
   g.w.elevation = map::placed_level(map::cell_elevation(map::cell_at(m, x, y)));
   g.anim = kNoAnim;
   follow();
   draw_sprite();
}

void reload_walker_sprite()
{
   if (g.slot == kNoSlot || g.sprite == nullptr)
   {
      return;
   }
   gfx::sprites::load(g.slot, *g.sprite);
   gfx::sprites::show(g.slot, g.w.visible);
   g.anim = kNoAnim;
   draw_sprite();
}

void set_object_visible(uint8_t local, bool on)
{
   if (local >= g.object_count)
   {
      return;
   }
   g_objects[local].visible = on;
   draw_object(local);
}

void set_template_cell(uint8_t local, int x, int y)
{
   if (local >= map::kMaxObjectTemplates)
   {
      return;
   }
   TemplateOverride& o = g_overrides[local];
   o.cell = true;
   o.x = static_cast<int16_t>(x);
   o.y = static_cast<int16_t>(y);
}

void set_template_movement(uint8_t local, map::Movement movement, uint8_t arg, uint8_t flags, uint8_t facing)
{
   if (local >= map::kMaxObjectTemplates)
   {
      return;
   }
   TemplateOverride& o = g_overrides[local];
   o.movement = true;
   o.kind = movement;
   o.arg = arg;
   o.flags = flags;
   o.facing = facing;
}

TemplateChange template_change(uint8_t local)
{
   TemplateChange c{};
   c.facing = NUM_DIRECTIONS;
   if (local >= map::kMaxObjectTemplates)
   {
      return c;
   }
   const TemplateOverride& o = g_overrides[local];
   c.cell = o.cell;
   c.movement = o.movement;
   c.x = o.x;
   c.y = o.y;
   c.kind = o.kind;
   c.arg = o.arg;
   c.flags = o.flags;
   c.facing = o.movement ? o.facing : static_cast<uint8_t>(NUM_DIRECTIONS);
   return c;
}

void set_template_change(uint8_t local, const TemplateChange& change)
{
   if (local >= map::kMaxObjectTemplates)
   {
      return;
   }
   if (change.cell)
   {
      set_template_cell(local, change.x, change.y);
   }
   if (change.movement)
   {
      set_template_movement(local, change.kind, change.arg, change.flags, change.facing);
   }
}

void set_home(uint8_t local, int x, int y)
{
   if (local >= map::kMaxObjectTemplates)
   {
      return;
   }
   g_objects[local].home_x = static_cast<int16_t>(x);
   g_objects[local].home_y = static_cast<int16_t>(y);
}

void show_walker(bool on)
{
   g.w.visible = on;
   draw_sprite();
}

void attach_sprite(uint8_t slot, const asset::Sprite* sprite)
{
   g.slot = slot;
   g.sprite = sprite;
   g.anim = kNoAnim;
   draw_sprite();
}

void update(const Intent& intent)
{
   Walker& w = g.w;
   g.frame = static_cast<uint8_t>(g.frame + 1);
   g.walker_began = false;
   if (map::current() != nullptr && g.state_seen != script::state_version())
   {
      refresh_pages();
   }
   update_approach();
   const bool standing = w.motion == Motion::Idle || w.motion == Motion::Turning;
   if (standing && !g.scripted && g_routes[kWalkerRoute].code != nullptr)
   {
      run_route(kWalkerRoute);
   }
   else if (standing && !g.scripted)
   {
      think(intent);
   }
   if (w.motion != Motion::Idle && w.motion != Motion::Turning)
   {
      int step = w.speed < w.remaining ? w.speed : w.remaining;
      if (g.routed && w.motion == Motion::Walking)
      {   // route steps: the route's speed level (walking unless a Speed step changed it: N2)
         step = pixels_at(g.walker_speed != 0 ? g.walker_speed : route::kDefaultSpeed);
         if (step > w.remaining)
         {
            step = w.remaining;
         }
      }
      w.px += dx_of(w.move_dir) * step;
      w.py += dy_of(w.move_dir) * step;
      w.remaining = static_cast<uint8_t>(w.remaining - step);
      if (w.motion == Motion::Jumping)
      {
         // Half a sine over the jump (32 pixels for a ledge).
         const int total = (w.x - g.from_x + w.y - g.from_y) * map::kMetatilePixels;
         const int len = total < 0 ? -total : (total == 0 ? kLedgeJumpPixels : total);
         const auto angle = static_cast<uint16_t>((len - w.remaining) * kHalfTurn / len);
         w.lift = static_cast<uint8_t>((sin_q14(angle) * kJumpHeight) >> kQ14Shift);
      }
      if (w.remaining == 0)
      {
         arrive();
         // Keep walking without a standing frame while the direction is held (I3: a class that keeps moving also
         // into another held direction).
         const bool keeps = (map::vehicle_class(w.vehicle_class).flags & map::kVehicleKeepsMoving) != 0;
         if (w.motion == Motion::Idle && !g.scripted && g_routes[kWalkerRoute].code == nullptr &&
             (intent.direction == w.facing || (keeps && intent.direction < NUM_DIRECTIONS)))
         {
            think(intent, true);
         }
      }
   }
   if (map::current() != nullptr)
   {
      update_objects();
      if (g.view_due && map::spawns_by_view(shown()))
      {
         update_view();   // I3: after this frame's moves, as the camera moves
      }
      else if (g.view_due)
      {
         respawn_cleared();   // C10: a map without a view looks over every object the same way
      }
   }
   g.view_due = false;
   update_camera();   // N2: after the objects, as the reference games move their camera object
   follow();
   draw_sprite();
   for (uint8_t i = 0; i < g.object_count; ++i)
   {
      draw_object(i);
   }
   doors::update();   // DS1: the doors animating on the map, one display frame
}

const Walker& walker()
{
   return g.w;
}

void set_vehicle(uint8_t vehicle)
{
   ENGINE_ASSERT(vehicle < (map::kWorld.vehicle_class_count > 0 ? map::kWorld.vehicle_class_count : 1));
   g.w.vehicle_class = vehicle;
   g.anim = kNoAnim;   // its animation set may differ: the next draw starts the right one
}

bool can_run()
{
   const map::Map* m = map::current();
   const bool map_allows = m == nullptr || (m->flags & map::kMapNoRun) == 0;
   return map_allows && (map::vehicle_class(g.w.vehicle_class).flags & map::kVehicleCanRun) != 0;
}

void set_action_rules(ActionRules rules)
{
   g_action_rules = rules;
}

void set_step_rule(StepRule rule)
{
   g_step_rule = rule;
}

ActionRules action_rules()
{
   return g_action_rules;
}

void set_noclip(bool on)
{
   g.w.noclip = on;
}

void set_encounters(bool on)
{
   g.encounters = on;
}

bool take_event(FieldEvent* out)
{
   if (g.event.what == Request::None)
   {
      return false;
   }
   if (out != nullptr)
   {
      *out = g.event;
   }
   g.event = FieldEvent{};
   return true;
}

bool has_event()
{
   return g.event.what != Request::None;
}

namespace
{

/// DS1: applies the map's override sets that persist and are on (their flag set, or their variable at its value).
/// @param index  the map entered (index into map::kWorld.maps)
void apply_persisted_sets(uint16_t index)
{
   for (uint16_t s = 0; s < map::kWorld.override_set_count; ++s)
   {
      const map::OverrideSet& o = map::kWorld.override_sets[s];
      bool on = false;
      if (o.map != index)
      {
         continue;
      }
      if (o.persist == map::OverridePersist::Flag)
      {
         on = script::flag(static_cast<script::FlagId>(o.id));
      }
      else if (o.persist == map::OverridePersist::Var)
      {
         on = script::var(static_cast<script::VarId>(o.id)) == o.value;
      }
      if (on)
      {
         map::set_override_set(s, true);
      }
   }
}

/// N2: a new map: the camera mover goes (the camera follows the walker again), the walker's fixed order and sprite
/// offset end and the effect slots are free (the objects' go with their spawn).
void reset_cutscene()
{
   g.camera_on = false;
   g.cam_left = 0;
   g.cam_rest_dx = 0;
   g.cam_rest_dy = 0;
   g_routes[kCameraRoute] = RouteRun{};
   g.walker_fixed = false;
   g.walker_dx = 0;
   g.walker_dy = 0;
   for (uint8_t i = 0; i < map::kMaxObjects; ++i)
   {
      if ((g.effect_slots >> i & 1u) != 0)
      {
         gfx::sprites::show(static_cast<uint8_t>(kFirstObjectSlot + i), false);
      }
   }
   g.effect_slots = 0;
}

/// A map's asset key (map::kWorld.keys).
/// @param index  index into map::kWorld.maps
/// @return its key, or AssetKey{0} when the world lists none (no map scripts run)
AssetKey key_of(uint16_t index)
{
   for (uint16_t i = 0; i < map::kWorld.map_count; ++i)
   {
      if (map::kWorld.keys[i].index == index)
      {
         return map::kWorld.keys[i].key;
      }
   }
   return AssetKey{0};
}

}   // namespace

namespace
{
/// enter() and enter_saved().
/// @param index   index into map::kWorld.maps
/// @param x       cell column
/// @param y       cell row
/// @param facing  Direction
/// @param saved   a saved game's restore steps, or nullptr: a warp's entry
void enter_with(uint16_t index, int x, int y, uint8_t facing, const SavedEntry* saved)
{
   ENGINE_ASSERT(index < map::kWorld.map_count);
   const map::Map& m = map::kWorld.maps[index];
   const map::Map* prev = map::current();
   const bool had_map = prev != nullptr || g.left;
   const uint16_t from_area = prev != nullptr ? prev->area : g.left_area;
   g.left = false;
   const Walker old = g.w;
   g.cam_dx = 0;
   g.cam_dy = 0;
   map::enter(m, static_cast<int32_t>(x) * map::kMetatilePixels + kCellCentre - hal::video::kScreenWidth / kHalf,
              static_cast<int32_t>(y) * map::kMetatilePixels + kCellCentre - hal::video::kScreenHeight / kHalf);
   place(x, y, facing);
   g.w.steps = old.steps;
   g.w.encounter_steps = old.encounter_steps;
   g.w.vehicle_class = old.vehicle_class;
   g.locked = false;
   g.object_count = 0;   // the old map's objects are gone; the new ones spawn after the load scripts
   clear_overrides();
   gfx::sprites::set_sorted(true);   // DS1: sprites cover each other by the world's draw-order rule
   doors::reset();   // DS1: the old map's doors stop (map::enter() dropped their cell overrides)
   if (had_map)
   {
      script::change_map(from_area, m.area);
   }
   reset_cutscene();   // N2: no camera mover, fixed orders, sprite offsets or effect slots on the new map
   apply_persisted_sets(index);   // DS1: before the load scripts, which may apply or clear sets themselves
   const AssetKey key = key_of(index);
   if (saved == nullptr)
   {
      script::enter_map(key);
   }
   else
   {
      if (saved->restore_view != nullptr)
      {
         saved->restore_view();   // C11: LoadSavedMapView, before RunOnLoadMapScript
      }
      script::enter_saved_map(key);
   }
   if (map::cell_edit_count() > 0)
   {
      map::redraw();   // N2: the map shows as its load scripts left its cells (they run after map::enter())
   }
   // C10: the field resumes (on_resume) before the objects spawn, as the reference games resume the map before they
   // place the player and the objects; the arrival's script (on_warp_into) runs after them
   script::run_map_scripts(key, script::TriggerKind::OnResume);
   spawn_objects();   // after on_transition / on_load / on_resume, which set hide and page flags
   if (saved == nullptr)
   {
      script::run_map_scripts(key, script::TriggerKind::OnWarpInto);
      return;
   }
   // C11: InitObjectEventsReturnToField: the saved objects (SpawnObjectEventsOnReturnToField), then on_return
   if (saved->restore_objects != nullptr)
   {
      saved->restore_objects();
   }
   script::run_map_scripts(key, script::TriggerKind::OnReturn);
}
}   // namespace

void enter(uint16_t index, int x, int y, uint8_t facing)
{
   enter_with(index, x, y, facing, nullptr);
}

void enter_saved(uint16_t index, int x, int y, uint8_t facing, const SavedEntry& hooks)
{
   enter_with(index, x, y, facing, &hooks);
}

void shift_map(uint16_t index, int dx, int dy)
{
   ENGINE_ASSERT(index < map::kWorld.map_count);
   const map::Map& m = map::kWorld.maps[index];
   const map::Map* prev = map::current();
   const uint16_t from_area = prev != nullptr ? prev->area : g.left_area;
   const int32_t shift_x = static_cast<int32_t>(dx) * map::kMetatilePixels;
   const int32_t shift_y = static_cast<int32_t>(dy) * map::kMetatilePixels;
   g.left = false;
   g.w.x = static_cast<int16_t>(g.w.x + dx);
   g.w.y = static_cast<int16_t>(g.w.y + dy);
   g.from_x = static_cast<int16_t>(g.from_x + dx);
   g.from_y = static_cast<int16_t>(g.from_y + dy);
   g.w.px += shift_x;
   g.w.py += shift_y;
   map::enter(m, map::camera_x() + shift_x, map::camera_y() + shift_y, true);
   g.object_count = 0;   // the old map's objects are gone; the new ones spawn after the load scripts
   follow();
   draw_sprite();
   clear_overrides();
   doors::reset();
   if (prev != nullptr)
   {
      script::change_map(from_area, m.area);
   }
   reset_cutscene();   // N2
   apply_persisted_sets(index);
   const AssetKey key = key_of(index);
   script::enter_map(key);
   if (map::cell_edit_count() > 0)
   {
      map::redraw();   // N2: as enter()
   }
   script::run_map_scripts(key, script::TriggerKind::OnResume);   // I4: a connection resumes the field too (C10: first)
   spawn_objects();
}

void leave()
{
   const map::Map* m = map::current();
   if (m != nullptr)
   {
      g.left_area = m->area;
      g.left = true;
   }
   map::leave();
   spawn_objects();   // no map: the object sprites go
}

void warp(const FieldEvent& e)
{
   ENGINE_ASSERT(e.what == Request::Warp);
   enter(e.map, e.x, e.y, g.w.facing);
}

void spawn_objects()
{
   const map::Map* m = map::current();
   g.object_count = m != nullptr ? m->object_count : 0;   // no map: every object slot is hidden
   ENGINE_ASSERT(g.object_count <= map::kMaxObjectTemplates);
   ENGINE_ASSERT(m == nullptr || map::spawns_by_view(*m) || g.object_count <= map::kMaxObjects);
   g.approach = kNoObject;
   g.talked = kNoObject;
   g.state_seen = script::state_version();
   g.slots_used = g.effect_slots;   // N2: slots claimed for effects (by the map's load scripts) stay theirs
   for (uint8_t& b : g.suppressed)
   {
      b = 0;
   }
   for (uint8_t i = 0; i < map::kMaxObjects; ++i)
   {
      if ((g.effect_slots >> i & 1u) == 0)
      {
         gfx::sprites::show(static_cast<uint8_t>(kFirstObjectSlot + i), false);
      }
   }
   for (uint8_t i = 0; i < map::kMaxObjectTemplates; ++i)
   {
      g_objects[i] = Object{};
      g_objects[i].slot = kNoSlot;
      g_routes[i] = RouteRun{};
   }
   if (m == nullptr)
   {
      return;
   }
   if (map::spawns_by_view(*m))
   {
      update_view();   // I3: those in view now; the others as the walker comes near
      return;
   }
   for (uint8_t i = 0; i < g.object_count; ++i)
   {
      g_objects[i].slot = claim_slot(i);   // slot 1 + local ID, kept while hidden
      const map::ObjectTemplate& t = m->objects[i];
      if (t.flag != map::kNoEventCondition && script::flag(script::FlagId{t.flag}))
      {
         draw_object(i);   // hidden: its slot off
         continue;
      }
      spawn(i);
   }
}

uint8_t object_count()
{
   return map::current() != nullptr ? g.object_count : 0;
}

const Object* object(uint8_t local)
{
   return local < object_count() ? &g_objects[local] : nullptr;
}

void show_object(uint8_t local, bool on)
{
   if (local >= g.object_count)
   {
      return;
   }
   set_suppressed(local, !on);
   if (on)
   {
      if (!g_objects[local].present)
      {
         if (g_objects[local].slot == kNoSlot)
         {
            g_objects[local].slot = claim_slot(local);
         }
         if (g_objects[local].slot != kNoSlot)
         {
            spawn(local);
         }
      }
      return;
   }
   despawn(local);
}

void stop_sight(uint8_t local)
{
   if (local < g.object_count)
   {
      g_objects[local].blind = true;
   }
}

void remove_object(uint8_t local)
{
   if (local >= g.object_count)
   {
      return;
   }
   set_suppressed(local, false);   // C10: the next look may bring it back
   despawn(local);
}

void face_object(uint8_t local, uint8_t dir)
{
   if (local >= g.object_count || dir >= NUM_DIRECTIONS || !g_objects[local].active)
   {
      return;
   }
   turn(g_objects[local].facing, g_objects[local].options, dir);
   draw_object(local);
}

bool move_object(uint8_t local, uint8_t dir, uint16_t steps)
{
   if (local >= g.object_count || dir >= NUM_DIRECTIONS || !g_objects[local].active)
   {
      return false;
   }
   Object& o = g_objects[local];
   o.next_dir = dir;
   if (o.moving)
   {   // finish the step under way first, then go on
      o.steps_left = steps;
      return true;
   }
   turn(o.facing, o.options, dir);
   if (steps == 0)
   {
      return true;
   }
   o.steps_left = static_cast<uint16_t>(steps - 1);
   begin_step(o, dir);
   return true;
}

bool object_path_clear(uint8_t local, uint8_t dir, uint16_t steps)
{
   if (local >= g.object_count || dir >= NUM_DIRECTIONS || !g_objects[local].active)
   {
      return false;
   }
   const Object& o = g_objects[local];
   for (int k = 1; k <= steps; ++k)
   {
      if (!may_enter(local, o.x + dx_of(dir) * k, o.y + dy_of(dir) * k))
      {
         return false;
      }
   }
   return true;
}

bool walker_path_clear(uint8_t dir, uint16_t steps)
{
   const Walker& w = g.w;
   if (dir >= NUM_DIRECTIONS)
   {
      return false;
   }
   if (w.noclip || (w.options & route::kOptionThrough) != 0)
   {
      return true;
   }
   int x = w.x;
   int y = w.y;
   uint8_t vehicle = w.vehicle_class;
   for (uint16_t k = 0; k < steps; ++k)
   {
      bool jump = false;
      Blocked b = check_step(x, y, dir, w.elevation, vehicle, &jump);
      // try_step()'s dismount rule: a class that dismounts steps onto land as class 0 (and walks on as it)
      if (b == Blocked::Terrain && vehicle != 0 &&
          (map::vehicle_class(vehicle).flags & map::kVehicleDismount) != 0)
      {
         bool foot_jump = false;
         if (check_step(x, y, dir, w.elevation, 0, &foot_jump) == Blocked::None)
         {
            b = Blocked::None;
            jump = foot_jump;
            vehicle = 0;
         }
      }
      // a scripted walk goes cell by cell: a ledge it would jump is no step it may take
      if (b != Blocked::None || jump)
      {
         return false;
      }
      x += dx_of(dir);
      y += dy_of(dir);
      if (object_on(x, y, w.elevation, kNoObject) != kNoObject)
      {
         return false;
      }
      if (g_step_rule != nullptr && g_step_rule(dir, x, y, false))
      {
         return false;   // N2: the game's step rule, checked without acting
      }
   }
   return true;
}

bool object_moving(uint8_t local)
{
   if (local == kCameraObject)
   {   // N2: the camera mover: its step or a script's route under way
      const RouteRun& r = g_routes[kCameraRoute];
      return g.camera_on && (g.cam_left > 0 || (r.code != nullptr && r.script));
   }
   if (local >= g.object_count || !g_objects[local].active)
   {
      return false;
   }
   const Object& o = g_objects[local];
   const RouteRun& r = g_routes[local];
   const bool route = r.code != nullptr && r.script && (r.code[0] & route::kRouteRepeat) == 0;
   return o.moving || o.steps_left > 0 || route;
}

bool route_object(uint8_t local, const uint8_t* code, bool from_script)
{
   if (local == kCameraObject)
   {   // N2: a route for the camera mover (none without it: the reference games' camera object must be spawned)
      if (!g.camera_on || code == nullptr)
      {
         return false;
      }
      start_route(kCameraRoute, code, from_script);
      return true;
   }
   if (local >= g.object_count || !g_objects[local].active || code == nullptr)
   {
      return false;
   }
   g_objects[local].steps_left = 0;
   start_route(local, code, from_script);
   return true;
}

void route_walker(const uint8_t* code)
{
   g.scripted = false;
   g.walker_steps = 0;
   g.walker_speed = route::kDefaultSpeed;   // N2: each route starts walking
   start_route(kWalkerRoute, code, true);
}

bool routes_busy()
{
   for (uint8_t i = 0; i < kRouteSlots; ++i)
   {
      const RouteRun& r = g_routes[i];
      if (i < kWalkerRoute && (i >= g.object_count || !g_objects[i].active))
      {
         continue;
      }
      if (i == kCameraRoute && !g.camera_on)
      {
         continue;
      }
      if (r.code != nullptr && r.script && (r.code[0] & route::kRouteRepeat) == 0)
      {
         return true;
      }
   }
   return g.scripted;
}

bool self_switch(uint8_t local, uint8_t letter)
{
   const int32_t bit = self_bit(local, letter);
   if (bit < 0)
   {
      return false;
   }
   return ((map::kWorld.self_switches[bit >> kByteOfBitShift] >> (bit & kBitInByteMask)) & 1) != 0;
}

void set_self_switch(uint8_t local, uint8_t letter, bool on)
{
   const int32_t bit = self_bit(local, letter);
   ENGINE_ASSERT(bit >= 0 && "this object has no self switches (the compiler gives them to objects that use them)");
   if (bit < 0)
   {
      return;
   }
   uint8_t& byte = map::kWorld.self_switches[bit >> kByteOfBitShift];
   const auto mask = static_cast<uint8_t>(1u << (bit & kBitInByteMask));
   const auto was = byte;
   byte = static_cast<uint8_t>(on ? (byte | mask) : (byte & ~mask));
   if (byte != was)
   {
      script::bump_state();
   }
}

uint16_t parallel_script(uint8_t local)
{
   if (local >= object_count())
   {
      return kNoScript;
   }
   const Object& o = g_objects[local];
   return o.active && o.trigger == map::Trigger::Parallel ? o.script : kNoScript;
}

void place_object(uint8_t local, int x, int y, uint8_t facing)
{
   if (local >= g.object_count || !g_objects[local].present)
   {
      return;
   }
   Object& o = g_objects[local];
   o.x = o.from_x = static_cast<int16_t>(x);
   o.y = o.from_y = static_cast<int16_t>(y);
   o.px = static_cast<int32_t>(x) * map::kMetatilePixels;
   o.py = static_cast<int32_t>(y) * map::kMetatilePixels;
   o.moving = false;
   o.steps_left = 0;
   o.lift = 0;
   // A script's route stops; a Custom object's own route starts over from the new cell.
   start_route(local, o.movement == map::Movement::Custom ? map_route(o.route) : nullptr, false);
   if (facing < NUM_DIRECTIONS)
   {
      turn(o.facing, o.options, facing);
   }
   draw_object(local);
}

void set_camera_offset(int32_t dx, int32_t dy)
{
   g.cam_dx = dx;
   g.cam_dy = dy;
   if (map::current() != nullptr)
   {
      follow();
      draw_sprite();
      for (uint8_t i = 0; i < g.object_count; ++i)
      {
         draw_object(i);
      }
   }
}

void face_walker(uint8_t dir)
{
   if (dir < NUM_DIRECTIONS)
   {
      turn(g.w.facing, g.w.options, dir);
   }
}

void move_walker(uint8_t dir, uint16_t steps)
{
   if (dir >= NUM_DIRECTIONS)
   {
      return;
   }
   Walker& w = g.w;
   const bool standing = w.motion == Motion::Idle || w.motion == Motion::Turning;
   if (standing)
   {
      turn(w.facing, w.options, dir);
   }
   if (steps == 0)
   {
      return;
   }
   w.running = false;
   g.scripted = true;
   g.walker_dir = dir;
   g_routes[kWalkerRoute] = RouteRun{};
   if (standing)
   {
      g.walker_steps = static_cast<uint16_t>(steps - 1);
      start_move(dir, 1, kWalkSpeed, Motion::Walking);
   }
   else
   {   // after the step under way
      g.walker_steps = steps;
   }
}

bool walker_moving()
{
   const RouteRun& r = g_routes[kWalkerRoute];
   return g.scripted || r.code != nullptr;
}

void lock()
{
   g.locked = true;
}

void release()
{
   if (g.event.what != Request::None || g.approach != kNoObject)
   {
      return;
   }   // another script is on its way
   g.locked = false;
   if (map::current() == nullptr)
   {
      return;
   }
   refresh_pages();
   for (uint8_t i = 0; i < g.object_count; ++i)
   {
      Object& o = g_objects[i];
      if (!o.active)
      {
         continue;
      }
      const bool keeps = o.movement == map::Movement::Static && (o.movement_flags & map::kStaticKeepsFacing) != 0;
      if (!o.moving && !keeps && (o.movement == map::Movement::Static || o.movement == map::Movement::Schedule))
      {
         turn(o.facing, o.options, o.base_facing);   // back to its direction after a talk or a cutscene
      }
   }
   // I3: the object a script was requested for starts its movement over (its face action shows where it faces)
   if (g.talked < g.object_count && g_objects[g.talked].movement >= map::Movement::Turn &&
       g_objects[g.talked].movement != map::Movement::Mirror)
   {
      g_objects[g.talked].machine = kStart;
      g_objects[g.talked].in_place = 0;
      g_objects[g.talked].in_place_pace = kNoPace;
   }
   g.talked = kNoObject;
}

bool locked()
{
   return g.locked;
}

bool take_encounter(encounter::Result* out)
{
   if (!g.has_encounter)
   {
      return false;
   }
   if (out != nullptr)
   {
      *out = g.encounter;
   }
   g.has_encounter = false;
   return true;
}

void spawn_camera()
{
   g.camera_on = true;
   g.cam_x = g.w.x;
   g.cam_y = g.w.y;
   g.cam_px = g.w.px;
   g.cam_py = g.w.py;
   g.cam_left = 0;
   g.cam_speed = route::kDefaultSpeed;
   g.cam_facing = DIR_DOWN;   // the reference games' camera object faces down
   g.cam_options = route::kDefaultOptions;
   g_routes[kCameraRoute] = RouteRun{};
}

void remove_camera()
{
   if (!g.camera_on)
   {
      return;
   }
   // the view stays where the mover had it: its offset from the walker is kept until the next map
   g.cam_rest_dx += g.cam_px - g.w.px;
   g.cam_rest_dy += g.cam_py - g.w.py;
   g.camera_on = false;
   g.cam_left = 0;
   g_routes[kCameraRoute] = RouteRun{};
}

bool camera_spawned()
{
   return g.camera_on;
}

void set_fixed_order(uint8_t local, bool fixed, uint8_t order)
{
   if (local == script::kObjectPlayer)
   {
      g.walker_fixed = fixed;
      g.walker_order = order;
      return;
   }
   if (local < g.object_count)
   {
      g_objects[local].fixed_order = fixed;
      g_objects[local].order = order;
   }
}

void set_sprite_offset(uint8_t local, int16_t dx, int16_t dy)
{
   if (local == script::kObjectPlayer)
   {
      g.walker_dx = dx;
      g.walker_dy = dy;
      return;
   }
   if (local < g.object_count)
   {
      g_objects[local].sprite_dx = dx;
      g_objects[local].sprite_dy = dy;
   }
}

uint8_t claim_sprite_slot()
{
   const map::Map* m = map::current();
   const bool view = m != nullptr && map::spawns_by_view(*m);
   for (uint8_t n = 0; n < map::kMaxObjects; ++n)
   {
      const uint16_t bit = static_cast<uint16_t>(1u << n);
      if ((g.effect_slots & bit) != 0 || (view && (g.slots_used & bit) != 0) || (!view && n < g.object_count))
      {
         continue;   // an effect's, a spawned object's (view) or an object's own slot (no view)
      }
      g.effect_slots = static_cast<uint16_t>(g.effect_slots | bit);
      if (view)
      {
         g.slots_used = static_cast<uint16_t>(g.slots_used | bit);   // objects that spawn later skip it
      }
      return static_cast<uint8_t>(kFirstObjectSlot + n);
   }
   return kNoSpriteSlot;
}

void release_sprite_slot(uint8_t slot)
{
   if (slot < kFirstObjectSlot || slot >= kFirstObjectSlot + map::kMaxObjects)
   {
      return;
   }
   const auto bit = static_cast<uint16_t>(1u << (slot - kFirstObjectSlot));
   if ((g.effect_slots & bit) == 0)
   {
      return;
   }
   g.effect_slots = static_cast<uint16_t>(g.effect_slots & ~bit);
   g.slots_used = static_cast<uint16_t>(g.slots_used & ~bit);
   gfx::sprites::unload(slot);   // C13a (U3): hidden and forgotten (a screen's return uploads no stale sprite)
}

}   // namespace capsule::field

/// The field's part of the script VM's world port (capsule/script/port.h).
namespace capsule::script::world
{

void set_vehicle(uint8_t vehicle)
{
   field::set_vehicle(vehicle);
}

}   // namespace capsule::script::world
