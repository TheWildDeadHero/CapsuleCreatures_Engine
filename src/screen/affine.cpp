/// @file
/// Affine sprite animation (see capsule/screen/affine.h).
#include "capsule/core/memory.h"
#include "capsule/screen/affine.h"

#include "capsule/core/assert.h"
#include "capsule/core/trig.h"
#include "capsule/core/types.h"
#include "capsule/screen/sprites.h"

namespace capsule::screen::affine
{
namespace
{

namespace video = hal::video;

constexpr uint16_t kRotationLowMask = 0xFF;   ///< A step clears the rotation's low byte.
constexpr int kAnchorShift = 9;               ///< The anchor rule's rounding term: (a1 - a0) >> 9.
constexpr int kAnchorDimensionShift = 8;      ///< The anchor rule's a0: dimension << 8.
constexpr int kAnchorInverseShift = 16;       ///< The anchor rule's a1: (dimension << 16) / scale parameter.

/// A matrix's identity.
constexpr Matrix kIdentity = {kScaleOne, 0, 0, kScaleOne};
/// A frame command that changes nothing (End's and an absolute frame's recompute).
constexpr Cmd kNoChange = {0, 0, 0, 0};

CAPSULE_EWRAM_BSS State g_states[kMatrices];     ///< Each matrix's animation state.
/// Each matrix's sprite's affine animations (attach()), or nullptr (EWRAM, as the other per-matrix tables: IWRAM is
/// the stack's).
CAPSULE_EWRAM_BSS const Cmd* const* g_anims[kMatrices] = {};
CAPSULE_EWRAM_BSS int16_t g_anchor_x[kMatrices];  ///< Each matrix's sprite's anchor x (set_anchor()), or kNoAnchor.
CAPSULE_EWRAM_BSS int16_t g_anchor_y[kMatrices];  ///< ... its anchor y.
uint32_t g_anchored = 0;        ///< Matrices whose sprite is anchored (bit per matrix).
CAPSULE_EWRAM_BSS Matrix g_matrices[kMatrices];  ///< Each matrix as last set.
uint32_t g_taken = 0;          ///< Taken matrices (bit per matrix).
bool g_enabled = true;         ///< Steps run.
MatrixRule g_rule = rotation_scaling;   ///< How a state becomes a matrix.

/// A state at its start: animation 0, scale 1, no rotation.
/// @return the state
constexpr State fresh_state()
{
   return State{0, 0, 0, 0, kScaleOne, kScaleOne, 0};
}

/// Whether a sprite is drawn with a matrix.
/// @param s  the sprite
/// @return true for kAffineNormal and kAffineDouble
bool is_affine(const sprites::Sprite& s)
{
   return s.affine_mode != sprites::kAffineOff;
}

/// The matrix whose state a sprite's animation uses (matrix 0 for a plain sprite, as the reference games').
/// @param s  the sprite
/// @return the matrix
uint8_t matrix_of(const sprites::Sprite& s)
{
   return is_affine(s) ? s.matrix : uint8_t{0};
}

/// One of a sprite's affine commands.
/// @param s      the sprite
/// @param anim   its animation
/// @param index  the command (as an 8-bit index)
/// @return the command
const Cmd& cmd_at(const sprites::Sprite& s, uint8_t anim, uint8_t index)
{
   return g_anims[matrix_of(s)][anim][index];
}

/// Whether a sprite's matrix is anchored.
/// @param s  the sprite
/// @return true when set_anchor() was called for it
bool anchored(const sprites::Sprite& s)
{
   return is_affine(s) && (g_anchored >> s.matrix & 1u) != 0;
}

/// The command a state points at.
/// @param s  the sprite
/// @param m  its matrix
/// @return the command
const Cmd& current(const sprites::Sprite& s, uint8_t m)
{
   return cmd_at(s, g_states[m].anim, g_states[m].cmd);
}

/// Adds a frame's changes to a state and sets its matrix.
/// @param m  the matrix
/// @param c  the frame (its first three fields)
void add_and_update(uint8_t m, const Cmd& c)
{
   State& st = g_states[m];
   st.x_scale = static_cast<int16_t>(st.x_scale + c.a);
   st.y_scale = static_cast<int16_t>(st.y_scale + c.b);
   // the change is a whole step of the rotation's top byte; the low byte is cleared each time
   st.rotation = static_cast<uint16_t>((st.rotation + (uint32_t{c.c} << kRotationShift)) & ~uint32_t{kRotationLowMask});
   set(m, g_rule(inverse_scale(st.x_scale), inverse_scale(st.y_scale), st.rotation));
}

/// Applies a frame as a command's first frame: with a duration, its changes (and one frame of it counted); without,
/// its values set at once.
/// @param m  the matrix
/// @param c  the frame
/// @return the frames left to count (the duration less one, or 0)
uint8_t apply_frame(uint8_t m, const Cmd& c)
{
   if (c.d != 0)
   {
      add_and_update(m, c);
      return static_cast<uint8_t>(c.d - 1);
   }
   State& st = g_states[m];
   st.x_scale = c.a;
   st.y_scale = c.b;
   st.rotation = static_cast<uint16_t>(uint32_t{c.c} << kRotationShift);
   add_and_update(m, kNoChange);
   return 0;
}

/// The anchor rule along one axis.
/// @param a0     dimension << 8
/// @param a1     (dimension << 16) / the scale parameter
/// @param coord  the anchor offset
/// @return the offset the sprite takes
int32_t anchor_coord(int32_t a0, int32_t a1, int32_t coord)
{
   const int32_t diff = a1 - a0;
   // rounds the difference's 1/512 toward zero on both sides of 0
   const int32_t term = diff < 0 ? (-diff) >> kAnchorShift : -(diff >> kAnchorShift);
   const uint32_t scaled = static_cast<uint32_t>(coord * a1) / static_cast<uint32_t>(a0);
   return coord - static_cast<int32_t>(scaled + static_cast<uint32_t>(term));
}

/// Moves an anchored sprite so its anchor stays while it scales.
/// @param s  the sprite
void update_anchor(sprites::Sprite& s)
{
   const Matrix& x = g_matrices[s.matrix];
   const int16_t ax = g_anchor_x[s.matrix];
   const int16_t ay = g_anchor_y[s.matrix];
   if (ax != kNoAnchor && x.pa != 0)
   {
      const int32_t dim = video::obj_width(s.tmpl->shape, s.tmpl->size);
      s.x2 = static_cast<int16_t>(anchor_coord(dim << kAnchorDimensionShift, (dim << kAnchorInverseShift) / x.pa,
                                               ax));
   }
   if (ay != kNoAnchor && x.pd != 0)
   {
      const int32_t dim = video::obj_height(s.tmpl->shape, s.tmpl->size);
      s.y2 = static_cast<int16_t>(anchor_coord(dim << kAnchorDimensionShift, (dim << kAnchorInverseShift) / x.pd,
                                               ay));
   }
}

void continue_anim(sprites::Sprite& s);

/// Goes back to the command after the previous loop marker while repeats are left.
/// @param s  the sprite
/// @param m  its matrix
void jump_to_loop_top(const sprites::Sprite& s, uint8_t m)
{
   State& st = g_states[m];
   if (st.loop == 0)
   {
      return;
   }
   --st.cmd;
   while (st.cmd != 0 && cmd_at(s, st.anim, static_cast<uint8_t>(st.cmd - 1)).a != kTypeLoop)
   {
      --st.cmd;
   }
   // one before the marker's next command: the step's increment lands on it (an 8-bit index wraps as stored)
   --st.cmd;
}

/// A Loop command: starts the loop's count or counts a repeat, then goes on in the same frame.
/// @param s  the sprite
/// @param m  its matrix
void loop_cmd(sprites::Sprite& s, uint8_t m)
{
   State& st = g_states[m];
   if (st.loop != 0)
   {
      --st.loop;
   }
   else
   {
      st.loop = static_cast<uint8_t>(current(s, m).b);
   }
   jump_to_loop_top(s, m);
   continue_anim(s);
}

/// The step of a sprite whose animation began: counts the frame's duration (applying its changes again), or runs
/// the next command.
/// @param s  the sprite
void continue_anim(sprites::Sprite& s)
{
   if (!is_affine(s))
   {
      return;
   }
   const uint8_t m = matrix_of(s);
   State& st = g_states[m];
   if (st.delay != 0)
   {
      if (!s.affine_paused)
      {
         --st.delay;
         add_and_update(m, current(s, m));
      }
   }
   else if (s.affine_paused)
   {
      return;
   }
   else
   {
      ++st.cmd;
      const Cmd& c = current(s, m);
      switch (c.a)
      {
      case kTypeLoop:
         loop_cmd(s, m);
         break;
      case kTypeJump:
         st.cmd = static_cast<uint8_t>(c.b);
         st.delay = apply_frame(m, current(s, m));
         break;
      case kTypeEnd:
         s.affine_ended = true;
         --st.cmd;
         add_and_update(m, kNoChange);
         break;
      default:
         st.delay = apply_frame(m, c);
         break;
      }
   }
   if (anchored(s))
   {
      update_anchor(s);
   }
}

/// The first step of an animation: its first command applied as a frame, its duration counted (nothing while the
/// sprite's animation 0 starts with End, as the reference games check).
/// @param s  the sprite
void begin_anim(sprites::Sprite& s)
{
   if (!is_affine(s) || cmd_at(s, 0, 0).a == kTypeEnd)
   {
      return;
   }
   const uint8_t m = matrix_of(s);
   State& st = g_states[m];
   st.cmd = 0;
   st.delay = 0;
   st.loop = 0;
   s.affine_beginning = false;
   s.affine_ended = false;
   st.delay = apply_frame(m, current(s, m));
   if (anchored(s))
   {
      update_anchor(s);
   }
}

/// Installs the step in the sprite module (once anything here is used).
void hook()
{
   sprites::set_affine_step(step);
}

}   // namespace

Matrix rotation_scaling(int16_t x_inverse, int16_t y_inverse, uint16_t rotation)
{
   // the rotation's top 8 bits pick the sine step (trig.h ignores the low byte)
   const int32_t sin = sin_q14(rotation);
   const int32_t cos = cos_q14(rotation);
   Matrix out{};
   out.pa = static_cast<int16_t>((x_inverse * cos) >> kQ14Shift);
   out.pb = static_cast<int16_t>((-x_inverse * sin) >> kQ14Shift);
   out.pc = static_cast<int16_t>((y_inverse * sin) >> kQ14Shift);
   out.pd = static_cast<int16_t>((y_inverse * cos) >> kQ14Shift);
   return out;
}

void set_matrix_rule(MatrixRule rule)
{
   g_rule = rule != nullptr ? rule : rotation_scaling;
}

void reset()
{
   g_taken = 0;
   g_anchored = 0;
   g_enabled = true;
   for (int m = 0; m < kMatrices; ++m)
   {
      g_states[m] = fresh_state();
      g_anims[m] = nullptr;
      set(static_cast<uint8_t>(m), kIdentity);
   }
   hook();
}

uint8_t alloc_matrix()
{
   for (int m = 0; m < kMatrices; ++m)
   {
      const uint32_t bit = uint32_t{1} << m;
      if ((g_taken & bit) == 0)
      {
         g_taken |= bit;
         return static_cast<uint8_t>(m);
      }
   }
   return kNoMatrix;
}

void free_matrix(uint8_t m)
{
   ENGINE_ASSERT(m < kMatrices);
   g_taken &= ~(uint32_t{1} << m);
   g_anchored &= ~(uint32_t{1} << m);
   g_anims[m] = nullptr;
   set(m, kIdentity);
}

bool taken(uint8_t m)
{
   return m < kMatrices && (g_taken >> m & 1u) != 0;
}

void set(uint8_t m, const Matrix& x)
{
   ENGINE_ASSERT(m < kMatrices);
   g_matrices[m] = x;
   sprites::set_matrix(m, x.pa, x.pb, x.pc, x.pd);
}

Matrix get(uint8_t m)
{
   ENGINE_ASSERT(m < kMatrices);
   return g_matrices[m];
}

void set_rotation_scaling(uint8_t m, int16_t x_scale, int16_t y_scale, uint16_t rotation)
{
   set(m, g_rule(inverse_scale(x_scale), inverse_scale(y_scale), rotation));
}

State& state(uint8_t m)
{
   ENGINE_ASSERT(m < kMatrices);
   return g_states[m];
}

uint8_t attach(uint8_t id, const Cmd* const* anims)
{
   sprites::Sprite& s = sprites::sprite(id);
   const uint8_t m = alloc_matrix();
   if (m == kNoMatrix)
   {
      return kNoMatrix;
   }
   hook();
   if (s.affine_mode == sprites::kAffineOff)
   {
      s.affine_mode = sprites::kAffineNormal;
   }
   g_anims[m] = anims;
   g_anchored &= ~(uint32_t{1} << m);
   s.matrix = m;
   s.affine_beginning = true;
   s.affine_ended = false;
   sprites::set_center(s);
   g_states[m] = fresh_state();
   return m;
}

void prepare(uint8_t id, const void* arg)
{
   attach(id, static_cast<const Cmd* const*>(arg));
}

void start(uint8_t id, uint8_t anim)
{
   sprites::Sprite& s = sprites::sprite(id);
   const uint8_t m = matrix_of(s);
   g_states[m] = fresh_state();
   g_states[m].anim = anim;
   s.affine_beginning = true;
   s.affine_ended = false;
}

void start_if_different(uint8_t id, uint8_t anim)
{
   if (g_states[matrix_of(sprites::sprite(id))].anim != anim)
   {
      start(id, anim);
   }
}

void change(uint8_t id, uint8_t anim)
{
   sprites::Sprite& s = sprites::sprite(id);
   g_states[matrix_of(s)].anim = anim;
   s.affine_beginning = true;
   s.affine_ended = false;
}

void set_anims(uint8_t id, const Cmd* const* anims)
{
   g_anims[matrix_of(sprites::sprite(id))] = anims;
}

void detach(uint8_t id)
{
   sprites::Sprite& s = sprites::sprite(id);
   if (is_affine(s))
   {
      free_matrix(s.matrix);
      s.affine_mode = sprites::kAffineOff;
   }
}

void set_mode(uint8_t id, uint8_t mode)
{
   ENGINE_ASSERT(mode == sprites::kAffineOff || mode == sprites::kAffineNormal || mode == sprites::kAffineDouble);
   sprites::Sprite& s = sprites::sprite(id);
   s.affine_mode = mode;
   sprites::set_center(s);
}

void set_anchor(uint8_t id, int16_t x, int16_t y)
{
   const sprites::Sprite& s = sprites::sprite(id);
   if (!is_affine(s))
   {
      return;
   }
   g_anchor_x[s.matrix] = x;
   g_anchor_y[s.matrix] = y;
   g_anchored |= uint32_t{1} << s.matrix;
}

void set_enabled(bool on)
{
   g_enabled = on;
}

void step(sprites::Sprite& s)
{
   if (!g_enabled || !is_affine(s) || g_anims[s.matrix] == nullptr)
   {
      return;
   }
   if (s.affine_beginning)
   {
      begin_anim(s);
   }
   else
   {
      continue_anim(s);
   }
}

}   // namespace capsule::screen::affine
