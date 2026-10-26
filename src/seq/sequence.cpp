/// @file
/// The sequence player (see capsule/seq/sequence.h).
#include "capsule/seq/sequence.h"

#include "capsule/core/assert.h"
#include "capsule/seq/sequence_port.h"

namespace capsule::seq
{
namespace
{

constexpr Q8Wide kLevelMax = kLevelFull << kQ8Shift;   ///< Brightness / blend weight / palette coefficient 16, in Q8.

/// @name Operand unpacking
///@{
constexpr uint32_t kLow16Mask = 0xFFFF;   ///< Bits 15:0 of an operand.
constexpr int kHigh16Shift = 16;   ///< Shift of bits 31:16.
constexpr uint32_t kByteMask = 0xFF;   ///< One byte of an operand.
constexpr int kOperandByteBits = 8;   ///< Bits per operand byte.
constexpr uint8_t kBgIndexMask = kMaxBgs - 1;   ///< BgPos / BgScroll: the layer bits of operand a.
constexpr uint8_t kWindowIndexMask = 1;   ///< Window / WindowMove: the window bit of operand a.
constexpr int kTextTargetMask = 7;   ///< Text / TextClear: TextTarget bits of operand a.
constexpr int kTextAlignShift = 3;   ///< Text: shift of TextAlign in operand a.
constexpr int kTextAlignMask = 3;   ///< Text: TextAlign bits (after the shift).
constexpr int kTextTypedShift = 5;   ///< Text: shift of the typed bit in operand a.
///@}

/// @name TextBox operand bytes (cell rect x | y << 8 | w << 16 | h << 24)
///@{
constexpr int kBoxByteX = 0;   ///< Left.
constexpr int kBoxByteY = 1;   ///< Top.
constexpr int kBoxByteW = 2;   ///< Width.
constexpr int kBoxByteH = 3;   ///< Height.
///@}

/// @name advance_motion() kinds
///@{
constexpr int kMotionSprite = 0;   ///< A sprite slot.
constexpr int kMotionLayer = 1;   ///< A background layer.
constexpr int kMotionMap = 2;   ///< The map camera.
///@}
constexpr FadeMask kFadeIn = flag_mask<FadeMask>(FADE_IN);   ///< FADE_IN mask.
constexpr FadeMask kFadeWhite = flag_mask<FadeMask>(FADE_WHITE);   ///< FADE_WHITE mask.

/// Asset key from a command operand.
/// @param raw_key  operand value
/// @return the key
inline AssetKey asset_key(uint32_t raw_key)
{
   return static_cast<AssetKey>(raw_key);
}

/// Whether a stage has a flag.
/// @param st  the stage
/// @param f   the flag
/// @return true if set
inline bool has_flag(const Stage& st, StageFlag f)
{
   return (st.flags & flag_mask<StageFlagMask>(f)) != 0;
}

/// Low half of an operand as a signed 16-bit value.
/// @param v  operand
/// @return bits 15:0, signed
inline int16_t lo16(uint32_t v)
{
   return static_cast<int16_t>(v & kLow16Mask);
}

/// High half of an operand.
/// @param v  operand
/// @return bits 31:16
inline uint16_t hi16(uint32_t v)
{
   return static_cast<uint16_t>(v >> kHigh16Shift);
}

/// One byte of an operand.
/// @param v  operand
/// @param n  byte 0 (lowest) to 3
/// @return the byte
inline uint8_t byte(uint32_t v, int n)
{
   return static_cast<uint8_t>((v >> (kOperandByteBits * n)) & kByteMask);
}

/// Whole pixels of a Q8 position.
/// @param v  Q8 position
/// @return pixels, truncated
inline int16_t px(Q8Wide v)
{
   return static_cast<int16_t>(v >> kQ8Shift);
}

/// @name Ramps
/// One division per ramp start; rare (P-12).
///@{

/// Sets a ramp to a value, idle.
/// @param r      the ramp
/// @param value  Q8 value
void ramp_set(Ramp& r, Q8Wide value)
{
   r.value = r.target = value;
   r.step = 0;
   r.left = 0;
}

/// Starts a ramp from its current value.
/// @param r       the ramp
/// @param target  Q8 value to reach
/// @param frames  duration; 0 jumps straight to @p target
void ramp_start(Ramp& r, Q8Wide target, uint16_t frames)
{
   if (frames == 0)
   {
      ramp_set(r, target);
      return;
   }
   r.target = target;
   r.step = (target - r.value) / frames;
   r.left = frames;
}

/// Advances a ramp one frame.
/// @param r  the ramp
/// @return true when the integer part changed (or the ramp just finished)
bool ramp_advance(Ramp& r)
{
   if (r.left == 0)
   {
      return false;
   }
   const Q8Wide before = r.value >> kQ8Shift;
   r.value += r.step;
   if (--r.left == 0)
   {
      r.value = r.target;
      return true;
   }
   return (r.value >> kQ8Shift) != before;
}

/// Integer part of a ramp.
/// @param r  the ramp
/// @return value >> 8
inline uint8_t ramp_level(const Ramp& r)
{
   return static_cast<uint8_t>(r.value >> kQ8Shift);
}
///@}

/// Sends a window's current rect to the port.
/// @param p  player
/// @param w  window 0 or 1
void push_window(Player& p, int w)
{
   const Ramp* r = p.window[w];
   port::window(static_cast<uint8_t>(w), p.window_flags[w], ramp_level(r[kRectX0]), ramp_level(r[kRectY0]),
                ramp_level(r[kRectX1]), ramp_level(r[kRectY1]));
}

/// Sends a sprite's current scale and angle to the port.
/// @param p     player
/// @param slot  affine slot
void push_affine(Player& p, int slot)
{
   port::sprite_affine(static_cast<uint8_t>(slot), static_cast<uint16_t>(p.affine[slot].scale.value >> kQ8Shift),
                       static_cast<uint16_t>(p.affine[slot].angle.value >> kQ8Shift));
}

/// Resets scale to 1.0 and angle to 0.
/// @param a  the slot's ramps
void reset_affine(Player::Affine& a)
{
   ramp_set(a.scale, kQ8One << kQ8Shift);
   ramp_set(a.angle, 0);
}

/// Clears sprites, motion, effects, text and the map view.
/// @param p  player
void clear_visuals(Player& p)
{
   for (auto& s : p.sprites)
   {
      s = {};
   }
   for (auto& a : p.affine)
   {
      reset_affine(a);
   }
   for (auto& b : p.bgs)
   {
      b = {};
   }
   p.map = {};
   port::sprites_clear();
   port::effects_clear();
   port::text_clear_all();
   port::map_show(AssetKey{0});
}

/// Enters p.stage: optional visual reset, tracks set to their first command.
/// @param p  player
void init_stage(Player& p)
{
   const Stage& st = p.seq->stages[p.stage];
   ENGINE_ASSERT(st.track_count >= 1 && st.track_count <= kMaxTracks);
   if (has_flag(st, STAGE_RESET_VISUALS))
   {
      clear_visuals(p);
   }
   for (int i = 0; i < kMaxTracks; ++i)
   {
      Player::Track& t = p.tracks[i];
      t = {};
      if (i < st.track_count)
      {
         t.base = p.seq->tracks[st.track_first + i];
         t.pc = t.base;
      }
      else
      {
         t.done = 1;
      }
   }
   p.stage_frames = 0;
   p.enter_pending = 0;
}

/// Follows a target: exits, or schedules the next stage for the next step().
/// @param p  player
/// @param t  target
/// @return false for kNoTarget, true otherwise
bool take_target(Player& p, Target t)
{
   if (t == kNoTarget)
   {
      return false;
   }
   if (is_exit(t))
   {
      p.status = Status::Exited;
      p.exit_code = exit_code_of(t);
   }
   else
   {
      ENGINE_ASSERT(t < p.seq->stage_count);
      p.stage = t;
      p.enter_pending = 1;
   }
   return true;
}

/// Resolves an asset operand, replacing kItemAsset with the current ForEach item.
/// @param t      the track
/// @param asset  operand
/// @return the asset key
AssetKey resolve_asset(const Player::Track& t, uint32_t asset)
{
   if (asset != kItemAsset)
   {
      return asset_key(asset);
   }
   for (int d = t.depth - 1; d >= 0; --d)
   {
      const Player::Loop& l = t.loops[d];
      if (l.list != 0)
      {
         return game::list_asset(l.list, l.i);
      }
   }
   ENGINE_ASSERT(false);   // the compiler rejects $item outside for_each
   return AssetKey{0};
}

/// Whether a sprite has stopped moving, scaling and rotating.
/// @param p     player
/// @param slot  sprite slot
/// @return true when idle (or out of range)
bool sprite_idle(const Player& p, uint8_t slot)
{
   if (slot >= kMaxSprites)
   {
      return true;
   }
   if (p.sprites[slot].moving)
   {
      return false;
   }
   if (slot < kMaxAffine)
   {
      return p.affine[slot].scale.left == 0 && p.affine[slot].angle.left == 0;
   }
   return true;
}

/// Whether a track's WaitFor condition is met.
/// @param p  player
/// @param t  the waiting track
/// @return true when the track may continue
bool wait_satisfied(const Player& p, const Player::Track& t)
{
   switch (static_cast<WaitKind>(t.wait_kind - 1))
   {
   case WaitKind::Fade:
      return p.fade.left == 0;
   case WaitKind::Sprite:
      return sprite_idle(p, static_cast<uint8_t>(t.wait_param));
   case WaitKind::Text:
      return !port::text_busy();
   case WaitKind::Input:
      return (p.pressed & t.wait_param) != 0;
   case WaitKind::SongEnd:
      return !port::song_playing();
   case WaitKind::SongFrame:
      return port::song_frame() >= t.wait_param;
   case WaitKind::Blend:
      return p.blend_src.left == 0 && p.blend_tgt.left == 0;
   case WaitKind::PalFade:
      return p.pal_coef.left == 0;
   case WaitKind::Window:
      for (const auto& w : p.window)
      {
         for (const auto& r : w)
         {
            if (r.left)
            {
               return false;
            }
         }
      }
      return true;
   case WaitKind::Cond:
      return game::cond(static_cast<uint8_t>(t.wait_param));
   default:
      ENGINE_ASSERT(false);
      return true;
   }
}

/// Opens a Repeat / ForEach, or skips it when it has no iterations.
/// @param t         the track
/// @param list      ForEach list id; 0 for Repeat
/// @param n         iterations
/// @param exit_rel  track-relative index after the loop
void start_loop(Player::Track& t, uint8_t list, uint16_t n, uint16_t exit_rel)
{
   if (n == 0)
   {
      t.pc = static_cast<uint16_t>(t.base + exit_rel);
      return;
   }
   ENGINE_ASSERT(t.depth < kMaxLoopDepth);
   Player::Loop& l = t.loops[t.depth++];
   l = {};
   l.body = t.pc;
   l.exit = static_cast<uint16_t>(t.base + exit_rel);
   l.n = n;
   l.list = list;
}

/// Runs one track until it blocks.
/// @param p  player
/// @param t  the track
/// @return the Goto target if one ran, else kNoTarget
Target run_track(Player& p, Player::Track& t)
{
   if (t.done)
   {
      return kNoTarget;
   }
   if (t.wait_kind)
   {
      if (!wait_satisfied(p, t))
      {
         return kNoTarget;
      }
      t.wait_kind = 0;
   }
   else if (t.wait > 0)
   {
      if (--t.wait > 0)
      {
         return kNoTarget;
      }
   }
   for (int budget = 0; budget < kMaxCmdsPerFrame; ++budget)
   {
      const Cmd& c = p.seq->cmds[t.pc++];
      switch (c.op)
      {
      case Op::End:
         t.done = 1;
         return kNoTarget;
      case Op::Wait:
         t.wait = c.b;
         if (t.wait > 0)
         {
            return kNoTarget;
         }
         break;
      case Op::WaitFor:
         t.wait_kind = static_cast<uint8_t>(c.a + 1);
         t.wait_param = c.b;
         return kNoTarget;
      case Op::Fade:
      {
         p.fade_white = (c.a & kFadeWhite) ? 1 : 0;
         if (c.a & kFadeIn)
         {
            ramp_set(p.fade, kLevelMax);
         }
         ramp_start(p.fade, (c.a & kFadeIn) ? 0 : kLevelMax, c.b);
         port::brightness(ramp_level(p.fade), p.fade_white);
         break;
      }
      case Op::SetVideoMode:
         port::set_video_mode(c.a);
         break;
      case Op::BgLoad:
         port::bg_load(c.a, resolve_asset(t, c.c));
         break;
      case Op::BgShow:
         port::bg_show(c.a, c.b != 0);
         break;
      case Op::BgPos:
      {
         Player::Motion& b = p.bgs[c.a & kBgIndexMask];
         b.x = static_cast<Q8Wide>(static_cast<int16_t>(c.b)) << kQ8Shift;
         b.y = static_cast<Q8Wide>(lo16(c.c)) << kQ8Shift;
         port::bg_offset(c.a, px(b.x), px(b.y));
         break;
      }
      case Op::BgScroll:
      {
         Player::Motion& b = p.bgs[c.a & kBgIndexMask];
         b.vx = static_cast<Q8>(c.b);
         b.vy = lo16(c.c);
         b.moving = (b.vx || b.vy) ? 1 : 0;
         break;
      }
      case Op::Blend:
         p.blend_mode = c.a;
         // b = source weight | target weight << 8, whole levels.
         ramp_set(p.blend_src, static_cast<Q8Wide>(c.b & kByteMask) << kQ8Shift);
         ramp_set(p.blend_tgt, static_cast<Q8Wide>(c.b >> kOperandByteBits) << kQ8Shift);
         port::blend(c.a, byte(c.c, 0), byte(c.c, 1));
         port::blend_weights(ramp_level(p.blend_src), ramp_level(p.blend_tgt));
         break;
      case Op::BlendRamp:
         ramp_start(p.blend_src, static_cast<Q8Wide>(c.b & kByteMask) << kQ8Shift, static_cast<uint16_t>(c.c));
         ramp_start(p.blend_tgt, static_cast<Q8Wide>(c.b >> kOperandByteBits) << kQ8Shift, static_cast<uint16_t>(c.c));
         break;
      case Op::SprLoad:
         port::sprite_load(c.a, resolve_asset(t, c.c));
         break;
      case Op::SprShow:
         port::sprite_show(c.a, c.b != 0);
         break;
      case Op::SprPos:
      {
         ENGINE_ASSERT(c.a < kMaxSprites);
         Player::Motion& s = p.sprites[c.a];
         s.x = static_cast<Q8Wide>(static_cast<int16_t>(c.b)) << kQ8Shift;
         s.y = static_cast<Q8Wide>(lo16(c.c)) << kQ8Shift;
         port::sprite_pos(c.a, px(s.x), px(s.y));
         break;
      }
      case Op::SprMove:
      {
         ENGINE_ASSERT(c.a < kMaxSprites);
         Player::Motion& s = p.sprites[c.a];
         s.vx = static_cast<Q8>(c.b);
         s.vy = lo16(c.c);
         s.frames = hi16(c.c);
         s.moving = (s.vx || s.vy) ? 1 : 0;
         break;
      }
      case Op::SprAnim:
         port::sprite_anim(c.a, c.b);
         break;
      case Op::SprObjWindow:
         port::sprite_obj_window(c.a, c.b != 0);
         break;
      case Op::SprAffine:
      {
         ENGINE_ASSERT(c.a < kMaxAffine);
         Player::Affine& a = p.affine[c.a];
         ramp_start(a.scale, static_cast<Q8Wide>(c.b) << kQ8Shift, hi16(c.c));
         ramp_start(a.angle, static_cast<Q8Wide>(c.c & kLow16Mask) << kQ8Shift, hi16(c.c));
         push_affine(p, c.a);
         break;
      }
      case Op::SprBlend:
         port::sprite_blend(c.a, c.b != 0);
         break;
      case Op::Emitter:
         port::emitter(c.a, c.b != 0, asset_key(c.c));
         break;
      case Op::PalAnim:
         port::palette_anim(c.a, c.b != 0, asset_key(c.c));
         break;
      case Op::PalFadeColor:
         p.pal_color = c.b;
         break;
      case Op::PalFade:
         p.pal_mask = c.c;
         p.pal_active_color = p.pal_color;
         ramp_start(p.pal_coef, static_cast<Q8Wide>(c.a) << kQ8Shift, c.b);
         port::palette_fade(p.pal_mask, p.pal_active_color, ramp_level(p.pal_coef));
         break;
      case Op::Scanline:
         port::scanline(c.a, c.b != 0, asset_key(c.c));
         break;
      case Op::Window:
      {
         const int w = c.a & kWindowIndexMask;
         p.window_flags[w] = c.b;
         for (int i = 0; i < kRectEdges; ++i)
         {
            ramp_set(p.window[w][i], static_cast<Q8Wide>(byte(c.c, i)) << kQ8Shift);
         }
         push_window(p, w);
         break;
      }
      case Op::WindowMove:
      {
         const int w = c.a & kWindowIndexMask;
         for (int i = 0; i < kRectEdges; ++i)
         {
            ramp_start(p.window[w][i], static_cast<Q8Wide>(byte(c.c, i)) << kQ8Shift, c.b);
         }
         break;
      }
      case Op::TextBox:
         port::text_box(c.a != 0, byte(c.c, kBoxByteX), byte(c.c, kBoxByteY), byte(c.c, kBoxByteW),
                        byte(c.c, kBoxByteH));
         break;
      case Op::TextStyle:
         p.text_style = c.a;
         break;
      case Op::Text:
         // a = TextTarget | TextAlign << 3 | typed << 5; b = x | y << 8.
         port::text_print(c.a & kTextTargetMask, static_cast<uint8_t>(c.b & kByteMask),
                          static_cast<uint8_t>(c.b >> kOperandByteBits), (c.a >> kTextAlignShift) & kTextAlignMask,
                          ((c.a >> kTextTypedShift) & 1) != 0, p.text_style, resolve_asset(t, c.c));
         break;
      case Op::TextClear:
         port::text_clear(c.a & kTextTargetMask, static_cast<uint8_t>(c.b & kByteMask),
                          static_cast<uint8_t>(c.b >> kOperandByteBits));
         break;
      case Op::MapShow:
         port::map_show(resolve_asset(t, c.c));
         break;
      case Op::MapCamera:
         p.map.x = static_cast<Q8Wide>(static_cast<int16_t>(c.b)) << kQ8Shift;
         p.map.y = static_cast<Q8Wide>(lo16(c.c)) << kQ8Shift;
         port::map_camera(px(p.map.x), px(p.map.y));
         break;
      case Op::MapScroll:
         p.map.vx = static_cast<Q8>(c.b);
         p.map.vy = lo16(c.c);
         p.map.moving = (p.map.vx || p.map.vy) ? 1 : 0;
         break;
      case Op::Song:
         port::song_play(resolve_asset(t, c.c), c.b);
         break;
      case Op::SongFade:
         port::song_fade(c.b);
         break;
      case Op::Sfx:
         port::sfx_play(resolve_asset(t, c.c));
         break;
      case Op::Jump:
         t.pc = static_cast<uint16_t>(t.base + c.b);
         break;
      case Op::If:
         if (game::cond(c.a) != (c.c != 0))
         {
            t.pc = static_cast<uint16_t>(t.base + c.b);
         }
         break;
      case Op::Repeat:
         start_loop(t, 0, c.b, static_cast<uint16_t>(c.c));
         break;
      case Op::ForEach:
      {
         uint16_t n = game::list_count(c.a);
         if (c.b != 0 && n > c.b)
         {
            n = c.b;
         }
         start_loop(t, c.a, n, static_cast<uint16_t>(c.c));
         break;
      }
      case Op::Next:
      {
         ENGINE_ASSERT(t.depth > 0);
         Player::Loop& l = t.loops[t.depth - 1];
         if (++l.i < l.n)
         {
            t.pc = l.body;
         }
         else
         {
            --t.depth;
         }
         break;
      }
      case Op::Goto:
         return c.a;
      default:
         ENGINE_ASSERT(false);
         t.done = 1;
         return kNoTarget;
      }
   }
   ENGINE_ASSERT(false);   // zero-time loop
   t.done = 1;
   return kNoTarget;
}

/// Moves one motion a frame and reports the new position to the port.
/// @param p      player (unused)
/// @param m      the motion
/// @param kind   0 = sprite, 1 = layer, 2 = map camera
/// @param index  sprite slot or layer
void advance_motion(Player& p, Player::Motion& m, int kind, uint8_t index)
{
   if (!m.moving)
   {
      return;
   }
   const int16_t ox = px(m.x), oy = px(m.y);
   m.x += m.vx;
   m.y += m.vy;
   const bool moved = px(m.x) != ox || px(m.y) != oy;
   if (kind == kMotionSprite)
   {
      port::sprite_pos(index, px(m.x), px(m.y));
      if (m.frames > 0 && --m.frames == 0)
      {
         m.moving = 0;
      }
   }
   else if (moved && kind == kMotionLayer)
   {
      port::bg_offset(index, px(m.x), px(m.y));
   }
   else if (moved && kind == kMotionMap)
   {
      port::map_camera(px(m.x), px(m.y));
   }
   (void)p;
}

/// Advances every motion and ramp one frame, pushing changes to the port.
/// @param p  player
void advance_all(Player& p)
{
   for (int i = 0; i < kMaxBgs; ++i)
   {
      advance_motion(p, p.bgs[i], kMotionLayer, static_cast<uint8_t>(i));
   }
   advance_motion(p, p.map, kMotionMap, 0);
   for (int i = 0; i < kMaxSprites; ++i)
   {
      advance_motion(p, p.sprites[i], kMotionSprite, static_cast<uint8_t>(i));
   }
   for (int i = 0; i < kMaxAffine; ++i)
   {
      const bool a = ramp_advance(p.affine[i].scale);
      const bool b = ramp_advance(p.affine[i].angle);
      if (a || b)
      {
         push_affine(p, i);
      }
   }
   if (ramp_advance(p.fade))
   {
      port::brightness(ramp_level(p.fade), p.fade_white);
   }
   const bool bs = ramp_advance(p.blend_src);
   const bool bt = ramp_advance(p.blend_tgt);
   if (bs || bt)
   {
      port::blend_weights(ramp_level(p.blend_src), ramp_level(p.blend_tgt));
   }
   if (ramp_advance(p.pal_coef))
   {
      port::palette_fade(p.pal_mask, p.pal_active_color, ramp_level(p.pal_coef));
   }
   for (int w = 0; w < kMaxWindows; ++w)
   {
      bool changed = false;
      for (auto& r : p.window[w])
      {
         changed = ramp_advance(r) || changed;
      }
      if (changed)
      {
         push_window(p, w);
      }
   }
}

/// Whether a combo fires this frame (see Combo).
/// @param c        the combo
/// @param held     keys held
/// @param pressed  keys newly pressed
/// @return true if it fires
bool combo_matches(const Combo& c, KeyBits held, KeyBits pressed)
{
   if (c.held == 0 && c.pressed == 0)
   {
      return false;
   }
   if ((held & c.held) != c.held)
   {
      return false;
   }
   if (c.pressed != 0 && (pressed & c.pressed) == 0)
   {
      return false;
   }
   return c.cond == 0 || game::cond(c.cond);
}

/// Runs one logical frame of the current stage.
/// @param p   player
/// @param st  the current stage
/// @return true if the stage was left
bool logical_frame(Player& p, const Stage& st)
{
   bool all_done = true;
   for (int i = 0; i < st.track_count; ++i)
   {
      const Target t = run_track(p, p.tracks[i]);
      if (t != kNoTarget && take_target(p, t))
      {
         return true;
      }
      all_done = all_done && p.tracks[i].done;
   }
   advance_all(p);
   ++p.stage_frames;
   if (has_flag(st, STAGE_EXIT_ON_SONG_END) && !port::song_playing() && take_target(p, st.on_song_end))
   {
      return true;
   }
   if (st.timeout_frames != 0 && p.stage_frames >= st.timeout_frames && take_target(p, st.on_timeout))
   {
      return true;
   }
   return all_done && take_target(p, st.on_done);
}

}   // namespace

void enter_stage(Player& p, uint8_t stage)
{
   ENGINE_ASSERT(stage < p.seq->stage_count);
   p.stage = stage;
   p.status = Status::Running;
   init_stage(p);
}

void start(Player& p, const Sequence& seq)
{
   p = {};
   p.seq = &seq;
   ramp_set(p.fade, 0);
   clear_visuals(p);
   enter_stage(p, seq.start_stage);
}

Status step(Player& p, KeyBits held, KeyBits pressed)
{
   if (p.status == Status::Exited)
   {
      return p.status;
   }
   if (p.enter_pending)
   {
      init_stage(p);
   }
   ++p.total_frames;
   const Stage& st = p.seq->stages[p.stage];

   if (has_flag(st, STAGE_SEED_ON_PRESS) && !p.seeded && pressed != 0)
   {
      game::rng_mix_entropy(p.total_frames);
      p.seeded = 1;
   }
   // No input on a stage's first frame: its setup commands always run first.
   if (p.stage_frames > 0)
   {
      for (uint8_t i = 0; i < st.combo_count; ++i)
      {
         const Combo& c = p.seq->combos[st.combo_first + i];
         if (combo_matches(c, held, pressed) && take_target(p, c.target))
         {
            return p.status;
         }
      }
      if ((pressed & st.skip_mask) != 0 && take_target(p, st.on_skip))
      {
         return p.status;
      }
   }

   int speed = 1;
   if (st.ff_mask != 0 && (held & st.ff_mask) == st.ff_mask && (st.ff_cond == 0 || game::cond(st.ff_cond)))
   {
      speed = st.ff_speed;
   }
   for (int s = 0; s < speed; ++s)
   {
      p.pressed = (s == 0) ? pressed : 0;
      if (logical_frame(p, st))
      {
         break;
      }
   }
   p.pressed = 0;
   return p.status;
}

}   // namespace capsule::seq
