/// @file
/// Move routes (D-17, RPG Maker XP's Set Move Route): a byte code of steps a
/// map object or the walker carries out, from a map object's page (Custom
/// movement) or from a script (`route ... { }`). tools/route_compiler.py
/// writes it; capsule::field runs it.
///
/// A route is a flags byte (kRouteRepeat, kRouteSkip) followed by steps, each
/// an opcode (RouteOp) and its argument bytes, ending with RouteOp::End.
/// Multi-byte arguments are little-endian.
///
/// | Op | Arguments | Does |
/// |---|---|---|
/// | End | — | ends the route (repeating routes start over) |
/// | Move | dir8, count8 | steps count cells in a direction |
/// | Random / Toward / Away | — | one step at random, towards or away from the walker |
/// | Forward / Backward | — | one step the way it faces, or backwards (facing kept) |
/// | Jump | dx8, dy8 (signed) | jumps to a cell up to 8 away (0, 0: in place) |
/// | Wait | frames16 | waits |
/// | Face | dir8 | turns |
/// | TurnRight / TurnLeft / TurnAround / TurnRandom | — | turns 90° right or left, 180° or at random |
/// | FaceToward / FaceAway | — | turns towards or away from the walker |
/// | Flag | flag16, on8 | sets or clears a flag |
/// | Speed | level8 | 1-4: 1 pixel every 2 frames, 1, 2 or 4 pixels a frame (N2: the walker's too, for the rest of its route) |
/// | Frequency | level8 | 1-5: how often its own movement acts (5 = at once) |
/// | Option | bit8 (kOption...), on8 | walk / step animation, direction fix, through, on top |
/// | Sprite | key32 | changes the sprite (an asset key) |
/// | Visible | on8 | shows or hides the sprite (it still blocks) |
/// | InPlace | dir8, speed8 | I4: faces the direction, walks in place (N2: animated) for a step's time at that speed |
/// | FaceOriginal | — | I4: turns back to its page's facing (the walker: nothing) |
/// | Game | step8, arg8 | I4: a game route step (field::game::route_step(); waits until it is done) |
///
/// Steps respect collision unless the mover's Through option is on. A blocked
/// step waits until it can be taken, or is skipped with kRouteSkip.
#pragma once

#include <cstdint>

/// Move routes.
namespace capsule::route
{

/// Route steps.
enum class RouteOp : uint8_t
{
   End,          ///< End of the route.
   Move,         ///< Steps in a direction.
   Random,       ///< One step at random.
   Toward,       ///< One step towards the walker.
   Away,         ///< One step away from the walker.
   Forward,      ///< One step the way it faces.
   Backward,     ///< One step backwards, facing kept.
   Jump,         ///< Jumps by a cell offset.
   Wait,         ///< Waits frames.
   Face,         ///< Turns to a direction.
   TurnRight,    ///< Turns 90° clockwise.
   TurnLeft,     ///< Turns 90° counter-clockwise.
   TurnAround,   ///< Turns 180°.
   TurnRandom,   ///< Turns to a random direction.
   FaceToward,   ///< Turns towards the walker.
   FaceAway,     ///< Turns away from the walker.
   Flag,         ///< Sets or clears a flag.
   Speed,        ///< Changes the movement speed.
   Frequency,    ///< Changes the movement frequency.
   Option,       ///< Turns a movement option on or off.
   Sprite,       ///< Changes the sprite.
   Visible,      ///< Shows or hides the sprite.
   InPlace,      ///< I4: walks in place facing a direction, for a step's time at a speed.
   FaceOriginal, ///< I4: turns back to its page's facing.
   Game,         ///< I4: a game route step (field::game::route_step()).
   NUM_ROUTE_OPS ///< Number of steps.
};

/// @name Route flags (the first byte)
///@{
inline constexpr uint8_t kRouteRepeat = 1;   ///< Starts over at the end (never finishes).
inline constexpr uint8_t kRouteSkip = 2;     ///< Skips blocked steps instead of waiting.
inline constexpr uint16_t kMinRouteBytes = 2;   ///< The shortest route: its flags byte and End.
///@}

/// @name Movement options (map objects' and the walker's), bits
///@{
inline constexpr uint8_t kOptionWalkAnim = 1;   ///< Walking animation while moving (default on).
inline constexpr uint8_t kOptionStepAnim = 2;   ///< Walking animation while standing too.
inline constexpr uint8_t kOptionDirFix = 4;     ///< Facing never changes.
inline constexpr uint8_t kOptionThrough = 8;    ///< Ignores collision and blocks nothing.
inline constexpr uint8_t kOptionOnTop = 16;     ///< Drawn in front of every layer.
inline constexpr uint8_t kDefaultOptions = kOptionWalkAnim;   ///< Options when none are given.
///@}

/// @name Speed and frequency levels
///@{
inline constexpr uint8_t kDefaultSpeed = 2;       ///< 1 pixel a frame (walking).
inline constexpr uint8_t kMaxSpeed = 4;           ///< 4 pixels a frame.
inline constexpr uint8_t kSpeedHalf = 1;          ///< 1 pixel every 2 frames.
inline constexpr uint8_t kSpeedDouble = 3;        ///< 2 pixels a frame.
inline constexpr int kSpeedDoublePixels = 2;      ///< Pixels a frame at kSpeedDouble.
inline constexpr int kMaxSpeedPixels = 4;         ///< Pixels a frame at kMaxSpeed.
inline constexpr uint8_t kDefaultFrequency = 3;   ///< Its own movement acts every 32-128 frames.
inline constexpr uint8_t kMaxFrequency = 5;       ///< Acts at once.
///@}

} // namespace capsule::route
