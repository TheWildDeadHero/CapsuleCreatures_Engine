/// @file
/// Presenter view: battle presenter cues on screen. See
/// capsule/battle/presenter_view.h.
#include "capsule/battle/presenter_view.h"

#include "capsule/asset/asset.h"
#include "capsule/core/memory.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/video.h"
#include "capsule/seq/sequence.h"

namespace capsule::battle::presenter_view
{

namespace
{

namespace video = hal::video;

/// Halves of a size: a sprite's centre is half its size from its corner.
constexpr int kHalf = 2;
/// The middle of the screen (Edge::Side picks the edge on the stand's half).
constexpr int kScreenMiddle = video::kScreenWidth / kHalf;
/// No track (Sequence::track).
constexpr int8_t kNoTrack = -1;
/// Sprite slots presenter_view owns: the presenters' and the sequences' (6-15).
constexpr int kOwnedSlots = gfx::sprites::kSlots - kFirstPresenterSlot;
static_assert(kFirstPresenterSlot + kPresenterSlots == kFirstSequenceSlot && kFirstSequenceSlot < gfx::sprites::kSlots);
/// Blend layers a fading presenter is blended over: everything but the sprites.
constexpr uint8_t kFadeBackLayers = video::kLayerAll & static_cast<uint8_t>(~video::kLayerObj);

/// What a presenter's sprite is doing.
enum class Doing : uint8_t
{
   Still,      ///< Standing (or hidden): nothing to animate.
   Sliding,    ///< Moving between from and to.
   Fading,     ///< Blending in or out.
   Sequence,   ///< Hidden while its sequence plays.
};

/// One presenter sprite slot.
struct Track
{
   const PresenterData* data;    ///< The presenter, or nullptr when the slot is unused.
   const asset::Sprite* sprite;  ///< Its sprite, or nullptr (text only).
   int16_t x;                    ///< Where it stands: top-left corner.
   int16_t y;                    ///< ...
   int16_t from_x;               ///< Start of the current slide.
   int16_t from_y;               ///< ...
   int16_t to_x;                 ///< End of the current slide.
   int16_t to_y;                 ///< ...
   uint8_t frames;               ///< Frames of the current motion.
   uint8_t frame;                ///< Frames done.
   Doing doing;                  ///< Current motion.
   bool leaving;                 ///< The motion ends with the presenter hidden.
};

/// The presenter sequence playing (one at a time).
struct SequenceState
{
   seq::Player player;   ///< The sequence player.
   int8_t track;         ///< Track whose sprite shows when it ends (kNoTrack: none).
   bool on;              ///< Playing.
   bool leaving;         ///< The track stays hidden afterwards.
};

/// Owner of the shared video blend while presenters fade. Every fading track
/// holds it; the last one to stop fading turns the blend off, so one fade
/// ending never cuts another off mid-way (and reset() always turns it off).
struct FadeBlend
{
   uint8_t fading;   ///< Tracks fading now.

   /// A track starts fading (fade_weight() then sets the blend).
   void acquire()
   {
      ++fading;
   }

   /// A track stops fading: the last one turns the blend off.
   void release()
   {
      if (fading > 0)
      {
         --fading;
      }
      if (fading == 0)
      {
         video::set_blend(video::BlendMode::Off, 0, 0);
      }
   }

   /// Forgets every fade and turns the blend off.
   void clear()
   {
      fading = 0;
      video::set_blend(video::BlendMode::Off, 0, 0);
   }
};

Track g_tracks[kPresenterSlots];                ///< Presenter slots 6-13.
CAPSULE_EWRAM_BSS SequenceState g_sequence;     ///< The sequence (its player is about 1 KB).
FadeBlend g_blend{};                            ///< The video blend, while any presenter fades.

/// Sprite slot of a track.
/// @param t  track index
/// @return gfx::sprites slot
uint8_t slot_of(int t)
{
   return static_cast<uint8_t>(kFirstPresenterSlot + t);
}

/// Width of a track's sprite.
/// @param tr  track
/// @return pixels (0 without a sprite)
int width_of(const Track& tr)
{
   return tr.sprite != nullptr ? video::obj_width(tr.sprite->shape, tr.sprite->size) : 0;
}

/// Height of a track's sprite.
/// @param tr  track
/// @return pixels (0 without a sprite)
int height_of(const Track& tr)
{
   return tr.sprite != nullptr ? video::obj_height(tr.sprite->shape, tr.sprite->size) : 0;
}

/// Where a slide starts (entering) or ends (leaving): just off one edge.
/// @param tr    track (x, y = where it stands)
/// @param edge  the edge
/// @param ox    off-screen x
/// @param oy    off-screen y
void off_screen(const Track& tr, Edge edge, int16_t& ox, int16_t& oy)
{
   ox = tr.x;
   oy = tr.y;
   if (edge == Edge::Side)
   {   // the edge on the stand's half of the screen
      edge = tr.x + width_of(tr) / kHalf < kScreenMiddle ? Edge::Left : Edge::Right;
   }
   switch (edge)
   {
   case Edge::Left:
      ox = static_cast<int16_t>(-width_of(tr));
      break;
   case Edge::Right:
      ox = static_cast<int16_t>(video::kScreenWidth);
      break;
   case Edge::Top:
      oy = static_cast<int16_t>(-height_of(tr));
      break;
   case Edge::Bottom:
      oy = static_cast<int16_t>(video::kScreenHeight);
      break;
   default:
      break;
   }
}

/// Shows or hides a track's sprite at a position.
/// @param t   track index
/// @param on  shown
/// @param x   left edge
/// @param y   top edge
void put_sprite(int t, bool on, int16_t x, int16_t y)
{
   if (g_tracks[t].sprite == nullptr)
   {
      return;
   }
   gfx::sprites::set_position(slot_of(t), x, y);
   gfx::sprites::show(slot_of(t), on);
}

/// Sets the blend for a fade at a weight.
/// @param weight  presenter weight, 0 (invisible) to kBlendMax (opaque)
void fade_weight(int weight)
{
   video::set_blend(video::BlendMode::Alpha, video::kLayerObj, kFadeBackLayers);
   video::set_blend_alpha(static_cast<uint8_t>(weight), static_cast<uint8_t>(video::kBlendMax - weight));
}

/// Stops a track's fade, if it fades: its sprite is opaque again and it
/// lets go of the blend.
/// @param t  track index
void end_fade(int t)
{
   Track& tr = g_tracks[t];
   if (tr.doing != Doing::Fading)
   {
      return;
   }
   gfx::sprites::set_mode(slot_of(t), video::ObjMode::Normal);   // fading tracks always have a sprite
   tr.doing = Doing::Still;
   g_blend.release();
}

/// Ends a track's motion.
/// @param t  track index
void finish(int t)
{
   Track& tr = g_tracks[t];
   end_fade(t);
   tr.doing = Doing::Still;
   put_sprite(t, !tr.leaving, tr.x, tr.y);
}

/// Starts a sequence cue.
/// @param t        track index
/// @param s        the sequence, or nullptr (then the cue is immediate)
/// @param leaving  the presenter stays hidden afterwards
void start_sequence(int t, const seq::Sequence* s, bool leaving)
{
   Track& tr = g_tracks[t];
   tr.leaving = leaving;
   if (s == nullptr || g_sequence.on)
   {   // nothing to play (or one is already playing): the cue takes no time
      finish(t);
      return;
   }
   put_sprite(t, false, tr.x, tr.y);
   tr.doing = Doing::Sequence;
   // Not seq::start(): that clears every visual, and the battle screen must stay.
   g_sequence.player = seq::Player{};
   g_sequence.player.seq = s;
   seq::enter_stage(g_sequence.player, s->start_stage);
   g_sequence.track = static_cast<int8_t>(t);
   g_sequence.on = true;
   g_sequence.leaving = leaving;
}

/// Starts one motion of a track.
/// @param t        track index
/// @param m        the motion
/// @param s        its sequence (Motion::Sequence), or nullptr
/// @param leaving  the presenter leaves (exit) rather than enters (enter, return)
void start_motion(int t, const PresenterMotion& m, const seq::Sequence* s, bool leaving)
{
   Track& tr = g_tracks[t];
   end_fade(t);   // a new cue during a fade replaces it
   tr.leaving = leaving;
   tr.frame = 0;
   tr.frames = m.frames;
   if (tr.sprite == nullptr || m.style == Motion::Stay || m.frames == 0)
   {   // nothing to animate: it is there (or gone, or stays) at once
      tr.leaving = leaving && m.style != Motion::Stay;
      finish(t);
      return;
   }
   switch (m.style)
   {
   case Motion::Slide:
   {
      int16_t ox = 0;
      int16_t oy = 0;
      off_screen(tr, m.from, ox, oy);
      tr.from_x = leaving ? tr.x : ox;
      tr.from_y = leaving ? tr.y : oy;
      tr.to_x = leaving ? ox : tr.x;
      tr.to_y = leaving ? oy : tr.y;
      tr.doing = Doing::Sliding;
      put_sprite(t, true, tr.from_x, tr.from_y);
      break;
   }
   case Motion::Fade:
      tr.doing = Doing::Fading;
      g_blend.acquire();
      gfx::sprites::set_mode(slot_of(t), video::ObjMode::SemiTransparent);
      fade_weight(leaving ? video::kBlendMax : 0);
      put_sprite(t, true, tr.x, tr.y);
      break;
   case Motion::Sequence:
      start_sequence(t, s, leaving);
      break;
   default:
      finish(t);
      break;
   }
}

/// Advances one track's slide or fade one frame.
/// @param t  track index
void advance(int t)
{
   Track& tr = g_tracks[t];
   if (tr.doing != Doing::Sliding && tr.doing != Doing::Fading)
   {
      return;
   }
   ++tr.frame;
   if (tr.frame >= tr.frames)
   {
      finish(t);
      return;
   }
   if (tr.doing == Doing::Sliding)
   {   // linear from the start to the end over the motion's frames
      const int x = tr.from_x + (tr.to_x - tr.from_x) * tr.frame / tr.frames;
      const int y = tr.from_y + (tr.to_y - tr.from_y) * tr.frame / tr.frames;
      put_sprite(t, true, static_cast<int16_t>(x), static_cast<int16_t>(y));
      return;
   }
   const int in = video::kBlendMax * tr.frame / tr.frames;   // weight of a fade in
   fade_weight(tr.leaving ? video::kBlendMax - in : in);
}

} // namespace

void reset()
{
   for (int t = 0; t < kPresenterSlots; ++t)
   {
      g_tracks[t] = Track{};
   }
   for (int k = 0; k < kOwnedSlots; ++k)
   {
      gfx::sprites::set_mode(slot_of(k), video::ObjMode::Normal);
      gfx::sprites::show(slot_of(k), false);
   }
   g_sequence.on = false;
   g_sequence.track = kNoTrack;
   g_blend.clear();
}

void begin(const State& s, const Event& e)
{
   if (e.kind != EventKind::Presenter || e.a >= kData.presenter_count || e.target >= s.party_count)
   {
      return;
   }
   const int t = e.target * kMaxPresenters + e.b;
   if (t >= kPresenterSlots)
   {
      return;   // more presenters than slots: this one is text only
   }
   const PresenterData& pd = kData.presenters[e.a];
   Track& tr = g_tracks[t];
   if (tr.data != &pd)
   {   // a new presenter in this slot: load its sprite and place it at its party's stand
      end_fade(t);   // the old one's fade (if any) lets go of the blend
      tr = Track{};
      tr.data = &pd;
      tr.sprite = raw(pd.sprite) != 0 ? asset::sprite(pd.sprite) : nullptr;
      if (tr.sprite != nullptr)
      {
         gfx::sprites::load(slot_of(t), *tr.sprite);
      }
      const FormatData& f = format(s);
      const uint8_t side = s.parties[e.target].side;
      tr.x = static_cast<int16_t>(f.stand_x[side] + pd.dx - width_of(tr) / kHalf);
      tr.y = static_cast<int16_t>(f.stand_y[side] + pd.dy - height_of(tr) / kHalf);
   }
   if (e.extra == kCueExit)
   {
      start_motion(t, pd.exit, pd.exit_seq, true);
   }
   else
   {   // kCueEnter and kCueReturn
      start_motion(t, pd.enter, pd.enter_seq, false);
   }
}

bool busy()
{
   if (g_sequence.on)
   {
      return true;
   }
   for (const Track& tr : g_tracks)
   {
      if (tr.doing != Doing::Still)
      {
         return true;
      }
   }
   return false;
}

void frame()
{
   for (int t = 0; t < kPresenterSlots; ++t)
   {
      advance(t);
   }
   if (g_sequence.on && seq::step(g_sequence.player, 0, 0) == seq::Status::Exited)
   {
      g_sequence.on = false;
      if (g_sequence.track != kNoTrack)
      {
         g_tracks[g_sequence.track].leaving = g_sequence.leaving;
         finish(g_sequence.track);
      }
      g_sequence.track = kNoTrack;
   }
}

} // namespace capsule::battle::presenter_view
