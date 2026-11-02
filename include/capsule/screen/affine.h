/// @file
/// Affine sprite animation (BF, D-38): command lists that scale and rotate a screen sprite (capsule/screen/
/// sprites.h) frame by frame through its affine matrix, as the reference games' sprite engine does, so their numbers
/// can be used as they are.
///
/// The model:
/// - Matrices: kMatrices OBJ matrices, taken first-free (alloc_matrix()) and given back (free_matrix(): identity).
/// - State per matrix (State: the animation, its command, the frames left, the loop count, x scale, y scale,
///   rotation), and the animations and anchor attach() / set_anchor() gave its sprite: sprites sharing a matrix
///   share them.
/// - Commands (Cmd, 6 bytes as the reference layout: two s16, two u8): a frame adds (x scale, y scale, rotation) to
///   the state, once a frame for its duration (duration 0: sets the state to its values at once); Loop (kTypeLoop)
///   repeats the commands since the previous Loop marker count more times; Jump (kTypeJump) goes to a command; End
///   (kTypeEnd) holds (each frame recomputes the matrix with no change). The first word of a command tells its kind:
///   a frame's x scale is never one of the three type values.
/// - The matrix of a state: the scales' inverses (0x10000 / scale, inverse_scale()) and the rotation's top 8 bits
///   through the matrix rule (rotation_scaling(): the engine's integer sine; a game may set another rule, e.g. one
///   that matches a system routine's table exactly: set_matrix_rule()).
/// - Each sprite with affine animations runs them after its frame animation in sprites::animate() (and in
///   sprites::create_and_animate()): begin (the first command applied, its duration counted) when it starts, then one
///   step a frame. Nothing runs while set_enabled(false).
/// - Double size (sprites::kAffineDouble): the sprite's box is twice its size, centred on the same point, so a scaled
///   up or rotated picture is not cut off; set_mode() changes a sprite's mode and its centre as the reference games'
///   CalcCenterToCornerVec.
#pragma once

#include "capsule/hal/video.h"

#include <cstdint>

namespace capsule::screen::sprites
{
struct Sprite;
}

/// Affine sprite animation.
namespace capsule::screen::affine
{

inline constexpr int kMatrices = hal::video::kNumObjAffine;   ///< OBJ matrices.
inline constexpr uint8_t kNoMatrix = 0xFF;                   ///< alloc_matrix() when every matrix is taken.
inline constexpr int16_t kScaleOne = 0x100;                  ///< Scale 1.0 (8.8 fixed point).
inline constexpr int16_t kTypeLoop = 0x7FFD;                 ///< Cmd::a of a Loop.
inline constexpr int16_t kTypeJump = 0x7FFE;                 ///< Cmd::a of a Jump.
inline constexpr int16_t kTypeEnd = 0x7FFF;                  ///< Cmd::a of an End.
inline constexpr int16_t kNoAnchor = 0x7FFF;                 ///< set_anchor(): this axis is not anchored.
inline constexpr int kRotationShift = 8;                     ///< A command's rotation is the state's top byte.

/// One affine animation command (data; the reference layout).
struct Cmd
{
   int16_t a;     ///< Frame: x scale change (8.8); else kTypeLoop / kTypeJump / kTypeEnd.
   int16_t b;     ///< Frame: y scale change (8.8); Loop: count; Jump: command index.
   uint8_t c;     ///< Frame: rotation change (1/256 turn steps, as a byte: 0xFF is -1).
   uint8_t d;     ///< Frame: duration in frames (0: set the values at once).
};

/// A frame command.
/// @param x_scale   x scale change, 8.8 (with duration 0: the x scale)
/// @param y_scale   y scale change, 8.8
/// @param rotation  rotation change, 1/256 turns
/// @param duration  frames (0: set at once)
/// @return the command
constexpr Cmd frame(int16_t x_scale, int16_t y_scale, int8_t rotation, uint8_t duration)
{
   return Cmd{x_scale, y_scale, static_cast<uint8_t>(rotation), duration};
}

/// A loop command (0: the loop's start marker).
/// @param count  repeats
/// @return the command
constexpr Cmd loop(int16_t count)
{
   return Cmd{kTypeLoop, count, 0, 0};
}

/// A jump command.
/// @param target  command index
/// @return the command
constexpr Cmd jump(int16_t target)
{
   return Cmd{kTypeJump, target, 0, 0};
}

/// The end command.
/// @return the command
constexpr Cmd end()
{
   return Cmd{kTypeEnd, 0, 0, 0};
}

/// One matrix's animation state.
struct State
{
   uint8_t anim;        ///< Animation playing.
   uint8_t cmd;         ///< Its command.
   uint8_t delay;       ///< Frames left of the command.
   uint8_t loop;        ///< Loop repeats left.
   int16_t x_scale;     ///< X scale, 8.8.
   int16_t y_scale;     ///< Y scale, 8.8.
   uint16_t rotation;   ///< Rotation, 16-bit turn (low byte 0 after a step).
};

/// An OBJ matrix (8.8).
struct Matrix
{
   int16_t pa;   ///< Texture dx per screen x.
   int16_t pb;   ///< Texture dx per screen y.
   int16_t pc;   ///< Texture dy per screen x.
   int16_t pd;   ///< Texture dy per screen y.
};

/// How a state's scales and rotation become a matrix.
/// @param x_inverse  0x10000 / x scale (inverse_scale())
/// @param y_inverse  0x10000 / y scale
/// @param rotation   16-bit turn (its top 8 bits count)
/// @return the matrix
using MatrixRule = Matrix (*)(int16_t x_inverse, int16_t y_inverse, uint16_t rotation);

/// The engine's matrix rule: pa = (x_inverse * cos) >> 14, pb = (-x_inverse * sin) >> 14, pc = (y_inverse * sin) >>
/// 14, pd = (y_inverse * cos) >> 14, with capsule/core/trig.h's Q14 sine of the rotation's top 8 bits.
/// @param x_inverse  8.8
/// @param y_inverse  8.8
/// @param rotation   16-bit turn
/// @return the matrix
Matrix rotation_scaling(int16_t x_inverse, int16_t y_inverse, uint16_t rotation);

/// Sets the matrix rule (reset() keeps it; nullptr: the engine's).
/// @param rule  the rule
void set_matrix_rule(MatrixRule rule);

/// A scale's inverse: 0x10000 / scale, kept to 16 bits (0 for 0).
/// @param scale  8.8
/// @return the inverse, 8.8
constexpr int16_t inverse_scale(int16_t scale)
{
   constexpr int32_t kOneSquared = 0x10000;
   return scale == 0 ? int16_t{0} : static_cast<int16_t>(kOneSquared / scale);
}

/// Frees every matrix (identity), resets every state, animations on (a scene's setup, after sprites::reset()).
void reset();

/// Takes the first free matrix.
/// @return it, or kNoMatrix
uint8_t alloc_matrix();

/// Gives a matrix back (identity).
/// @param m  0 to kMatrices - 1
void free_matrix(uint8_t m);

/// Whether a matrix is taken.
/// @param m  the matrix
/// @return true when it is
bool taken(uint8_t m);

/// Sets a matrix (it shows with the next frame).
/// @param m  the matrix
/// @param x  its values
void set(uint8_t m, const Matrix& x);

/// A matrix as last set.
/// @param m  the matrix
/// @return its values
Matrix get(uint8_t m);

/// Sets a matrix from scales and a rotation (the reference games' SetOamMatrixRotationScaling).
/// @param m         the matrix
/// @param x_scale   8.8
/// @param y_scale   8.8
/// @param rotation  16-bit turn
void set_rotation_scaling(uint8_t m, int16_t x_scale, int16_t y_scale, uint16_t rotation);

/// A matrix's animation state.
/// @param m  the matrix
/// @return its state
State& state(uint8_t m);

/// Gives a sprite affine animations: a matrix taken for it (when it has none of its own yet), its centre set for its
/// affine mode, its state reset, its animation 0 beginning at its next step (the reference games'
/// InitSpriteAffineAnim). An affine-off sprite is made affine (kAffineNormal).
/// @param id     the sprite
/// @param anims  its animations (each ends with End or Jump), or nullptr (a matrix only)
/// @return its matrix, or kNoMatrix when none was free (nothing changes)
uint8_t attach(uint8_t id, const Cmd* const* anims);

/// attach() as sprites::create_and_animate()'s prepare step (before the new sprite's callback).
/// @param id   the sprite
/// @param arg  its animations (const Cmd* const*), or nullptr
void prepare(uint8_t id, const void* arg);

/// Starts one of a sprite's affine animations at its next step (state reset to scale 1, rotation 0).
/// @param id    the sprite
/// @param anim  the animation
void start(uint8_t id, uint8_t anim);

/// start() unless that animation already plays.
/// @param id    the sprite
/// @param anim  the animation
void start_if_different(uint8_t id, uint8_t anim);

/// Changes a sprite's affine animation, keeping the state's scales and rotation (ChangeSpriteAffineAnim).
/// @param id    the sprite
/// @param anim  the animation
void change(uint8_t id, uint8_t anim);

/// Replaces the animations of a sprite's matrix, nothing else changing (the reference games' assignment of a
/// sprite's affineAnims; M5 P2). An affine-off sprite: matrix 0's (as the step's matrix_of()).
/// @param id     the sprite
/// @param anims  its animations, or nullptr
void set_anims(uint8_t id, const Cmd* const* anims);

/// Gives a sprite's matrix back and makes it a plain sprite (FreeSpriteOamMatrix).
/// @param id  the sprite
void detach(uint8_t id);

/// Changes a sprite's affine mode and its centre (kAffineOff, kAffineNormal or kAffineDouble).
/// @param id    the sprite
/// @param mode  the mode
void set_mode(uint8_t id, uint8_t mode);

/// Anchors a sprite while it scales: its x2 / y2 follow the scale so the anchor point stays (the reference games'
/// SetSpriteMatrixAnchor).
/// @param id  the sprite
/// @param x   anchor x offset from the centre, or kNoAnchor
/// @param y   anchor y offset, or kNoAnchor
void set_anchor(uint8_t id, int16_t x, int16_t y);

/// Turns every sprite's affine animation steps on or off.
/// @param on  run them
void set_enabled(bool on);

/// One sprite's affine step (sprites::animate() calls it after the frame animation).
/// @param s  the sprite
void step(sprites::Sprite& s);

}   // namespace capsule::screen::affine
