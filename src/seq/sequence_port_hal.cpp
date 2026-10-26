/// @file
/// The engine's sequence port: what the sequence player asks for, drawn with
/// the graphics modules and the video HAL (see capsule/seq/presenter.h).
///
/// | Calls | Implemented with |
/// |---|---|
/// | set_video_mode, bg_* | gfx::background (which picks each image's regular or affine form) |
/// | sprite_*, sprites_clear | gfx::sprites (OBJ window and semi-transparency as sprite modes) |
/// | brightness, blend, blend_weights | hal::video color effects (see apply_effects()) |
/// | window | hal::video windows 0 and 1 |
/// | palette_fade | gfx::palette software fade |
/// | text_* | text::layer (assets looked up in asset::kTable) |
/// | song_*, sfx_play | the audio system once started (capsule/seq/sequence_audio.h); else a silent timing stub |
/// | map_*, scanline, emitter, palette_anim | not yet: recorded in presenter::missing() |
#include "capsule/seq/sequence_port.h"

#include "capsule/asset/asset.h"
#include "capsule/gfx/background.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/slots.h"
#include "capsule/gfx/sprites.h"
#include "capsule/core/memory.h"
#include "capsule/hal/video.h"
#include "capsule/seq/presenter.h"
#include "capsule/seq/sequence.h"
#include "capsule/seq/sequence_audio.h"
#include "capsule/text/text_layer.h"

namespace capsule::seq
{
namespace
{

namespace video = hal::video;
using presenter::Missing;

static_assert(kMaxSprites <= gfx::slots::kSequences,
              "sequence sprite slots must fit the slot plan (capsule/gfx/slots.h)");
static_assert(kMaxAffine <= gfx::sprites::kAffineSlots, "affine sequence slots must fit the sprite module");

/// Stub song length before presenter::set_song_length(), frames (10 seconds).
constexpr uint32_t kDefaultSongFrames = 600;

/// Color effects and windows as the sequence set them.
struct Effects
{
   BlendMode mode;               ///< Scene blend mode.
   uint8_t first;              ///< Scene blend first targets.
   uint8_t second;             ///< Scene blend second targets.
   uint8_t eva;                ///< First weight (Brighten / Darken: the level).
   uint8_t evb;                ///< Second weight.
   uint8_t fade;               ///< Screen fade level, 0-16.
   uint8_t fade_white;         ///< 1 when fading to / from white.
   uint16_t win_flags[kMaxWindows];       ///< Window layer masks; 0 = window off.
   uint8_t win_rect[kMaxWindows][kRectEdges];     ///< Window rectangles.
   uint16_t obj_window_slots;   ///< Slots drawn as OBJ window masks.
   uint16_t semi_slots;         ///< Slots drawn semi-transparent.
};

Effects g_fx{};                      ///< Effects state.
presenter::MissingMask g_missing = 0;   ///< Features asked for but not shown.
AssetKey g_missing_key{0};           ///< Last missing asset.
uint32_t g_clock = 0;                ///< Frames since presenter::reset().
uint32_t g_song_length = kDefaultSongFrames;        ///< Stub song length; 0 = until stopped.
uint8_t g_song_on = 0;              ///< A song was started and not stopped.
uint32_t g_song_start = 0;           ///< g_clock when it started.
uint32_t g_song_end = 0;             ///< g_clock when it ends (0 with no length = never).
uint8_t g_song_audio = 0;           ///< The song plays on the audio system (not the stub).

/// Records a feature the port cannot show.
/// @param m  the feature
void note(Missing m)
{
   g_missing = static_cast<presenter::MissingMask>(g_missing | flag_mask<presenter::MissingMask>(m));
}

/// Records a missing asset.
/// @param key  its key
void note_asset(AssetKey key)
{
   note(presenter::MISSING_ASSET);
   g_missing_key = key;
}

/// Pushes blending and the screen fade to the HAL. The GBA has one color
/// effect at a time: while the screen fade is active it takes the effect
/// (brighten / darken every layer); semi-transparent sprites keep blending,
/// since the hardware blends them in any mode.
void apply_effects()
{
   if (g_fx.fade != 0)
   {
      video::set_blend(g_fx.fade_white ? video::BlendMode::Brighten : video::BlendMode::Darken, video::kLayerAll,
                       g_fx.second);
      video::set_blend_brightness(g_fx.fade);
   }
   else
   {
      const auto mode = static_cast<video::BlendMode>(g_fx.mode);
      video::set_blend(mode, g_fx.first, g_fx.second);
      video::set_blend_brightness(mode == video::BlendMode::Alpha ? 0 : g_fx.eva);
   }
   video::set_blend_alpha(g_fx.eva, g_fx.evb);
}

/// Pushes windows to the HAL. Inside a window: the layers in its flags;
/// outside every rectangle window: only the backdrop, with effects (so a
/// window reveals the scene and fades still apply). The OBJ window shows
/// everything; it changes nothing visible yet.
void apply_windows()
{
   const bool w0 = g_fx.win_flags[0] != 0;
   const bool w1 = g_fx.win_flags[1] != 0;
   const bool obj = g_fx.obj_window_slots != 0;
   for (int w = 0; w < kMaxWindows; ++w)
   {
      const uint8_t* r = g_fx.win_rect[w];
      video::set_window_rect(static_cast<uint8_t>(w), r[kRectX0], r[kRectY0], r[kRectX1], r[kRectY1]);
   }
   const uint8_t outside = (w0 || w1) ? video::kWindowEffects : video::kLayerAll;
   video::set_window_masks(static_cast<uint8_t>(g_fx.win_flags[0] & video::kLayerAll),
                           static_cast<uint8_t>(g_fx.win_flags[1] & video::kLayerAll), outside, video::kLayerAll);
   video::set_windows(w0, w1, obj);
}

/// Clamps a window edge to the screen.
/// @param v    edge
/// @param max  screen size
/// @return v limited to 0..max
uint8_t clamp_edge(uint8_t v, int max)
{
   return static_cast<uint8_t>(v > max ? max : v);
}

/// Sets a slot's sprite mode from the window and semi-transparency masks.
/// @param slot  the slot
void apply_sprite_mode(uint8_t slot)
{
   const auto bit = static_cast<uint16_t>(1u << slot);
   video::ObjMode mode = video::ObjMode::Normal;
   if (g_fx.obj_window_slots & bit)
   {
      mode = video::ObjMode::Window;
   }
   else if (g_fx.semi_slots & bit)
   {
      mode = video::ObjMode::SemiTransparent;
   }
   gfx::sprites::set_mode(slot, mode);
}

} // namespace

namespace presenter
{

void reset()
{
   gfx::palette::reset();
   gfx::background::reset();
   gfx::sprites::reset();
   text::layer::reset();
   g_fx = Effects{};
   apply_effects();
   apply_windows();
   g_missing = 0;
   g_missing_key = AssetKey{0};
   g_clock = 0;
   g_song_on = 0;
   g_song_audio = 0;
}

void frame()
{
   ++g_clock;
   gfx::sprites::update();
   text::layer::update();
}

void set_song_length(uint32_t frames)
{
   g_song_length = frames;
}

MissingMask missing()
{
   return g_missing;
}

AssetKey last_missing_asset()
{
   return g_missing_key;
}

} // namespace presenter

namespace port
{

// --- video ---
void set_video_mode(uint8_t mode)
{
   gfx::background::set_display_mode(mode);
}

void bg_load(uint8_t bg, AssetKey asset)
{
   const asset::Background* image = asset::background(asset);
   if (image == nullptr || !gfx::background::load(bg, *image))
   {
      note_asset(asset);
   }
}

void bg_show(uint8_t bg, bool on)
{
   gfx::background::show(bg, on);
}

void bg_offset(uint8_t bg, int16_t x, int16_t y)
{
   gfx::background::set_position(bg, x, y);
}

void blend(uint8_t mode, uint8_t src_layers, uint8_t tgt_layers)
{
   g_fx.mode = static_cast<BlendMode>(mode < static_cast<uint8_t>(BlendMode::NUM_BLEND_MODES) ? mode : 0);
   g_fx.first = src_layers;
   g_fx.second = tgt_layers;
   apply_effects();
}

void blend_weights(uint8_t src, uint8_t tgt)
{
   g_fx.eva = src;
   g_fx.evb = tgt;
   apply_effects();
}

void brightness(uint8_t level, bool white)
{
   g_fx.fade = level > kLevelFull ? kLevelFull : level;
   g_fx.fade_white = white ? 1 : 0;
   apply_effects();
}

void palette_fade(uint32_t mask, uint16_t color, uint8_t coef)
{
   gfx::palette::set_fade(mask, color, coef > gfx::palette::kFadeFull ? gfx::palette::kFadeFull : coef);
}

void window(uint8_t win, uint16_t flags, uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
   const int w = win & 1;
   x1 = clamp_edge(x1, video::kScreenWidth);
   y1 = clamp_edge(y1, video::kScreenHeight);
   x0 = x0 > x1 ? x1 : x0;
   y0 = y0 > y1 ? y1 : y0;
   g_fx.win_flags[w] = flags;
   uint8_t* r = g_fx.win_rect[w];
   r[kRectX0] = x0;
   r[kRectY0] = y0;
   r[kRectX1] = x1;
   r[kRectY1] = y1;
   apply_windows();
}

// --- sprites ---
void sprite_load(uint8_t slot, AssetKey asset)
{
   const asset::Sprite* sprite = asset::sprite(asset);
   if (sprite == nullptr)
   {
      note_asset(asset);
      return;
   }
   gfx::sprites::load(slot, *sprite);
}

void sprite_show(uint8_t slot, bool on)
{
   gfx::sprites::show(slot, on);
}

void sprite_pos(uint8_t slot, int16_t x, int16_t y)
{
   gfx::sprites::set_position(slot, x, y);
}

void sprite_anim(uint8_t slot, uint16_t anim)
{
   gfx::sprites::play(slot, anim);
}

void sprite_obj_window(uint8_t slot, bool on)
{
   const auto bit = static_cast<uint16_t>(1u << slot);
   g_fx.obj_window_slots = static_cast<uint16_t>(on ? (g_fx.obj_window_slots | bit) : (g_fx.obj_window_slots & ~bit));
   apply_sprite_mode(slot);
   apply_windows();
}

void sprite_affine(uint8_t slot, uint16_t scale_q8, uint16_t angle)
{
   gfx::sprites::set_transform(slot, scale_q8, angle);
}

void sprite_blend(uint8_t slot, bool on)
{
   const auto bit = static_cast<uint16_t>(1u << slot);
   g_fx.semi_slots = static_cast<uint16_t>(on ? (g_fx.semi_slots | bit) : (g_fx.semi_slots & ~bit));
   apply_sprite_mode(slot);
}

void sprites_clear()
{
   gfx::sprites::reset();
   g_fx.obj_window_slots = 0;
   g_fx.semi_slots = 0;
   apply_windows();
}

// --- effects ---
void emitter(uint8_t, bool on, AssetKey)
{
   if (on)
   {
      note(presenter::MISSING_EMITTER);
   }
}

void palette_anim(uint8_t, bool on, AssetKey)
{
   if (on)
   {
      note(presenter::MISSING_PALETTE_ANIM);
   }
}

void scanline(uint8_t, bool on, AssetKey)
{
   if (on)
   {
      note(presenter::MISSING_SCANLINE);
   }
}

void effects_clear()
{
}   // emitters, palette animations and scanline effects do not run yet

// --- text ---
void text_box(bool open, uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
   if (open)
   {
      text::layer::open_box(x, y, w, h);
   }
   else
   {
      text::layer::close_box();
   }
}

void text_print(uint8_t target, uint8_t x, uint8_t y, uint8_t align, bool typed, uint8_t style, AssetKey text)
{
   const text::Code* codes = asset::text(text);
   if (codes == nullptr)
   {
      note_asset(text);
      return;
   }
   text::layer::set_style(style);
   const auto a =
      static_cast<text::layer::Align>(align < static_cast<uint8_t>(text::layer::Align::NUM_ALIGNS) ? align : 0);
   text::layer::print(target, x, y, a, typed, codes);
}

void text_clear(uint8_t target, uint8_t first_row, uint8_t rows)
{
   text::layer::clear(target, first_row, rows);
}

bool text_busy()
{
   return text::layer::busy();
}

void text_clear_all()
{
   text::layer::clear_all();
}

// --- view-only map renderer (not yet: C-03) ---
void map_show(AssetKey map)
{
   if (raw(map) != 0)
   {
      note(presenter::MISSING_MAP_VIEW);
   }
}

void map_camera(int16_t, int16_t)
{
   note(presenter::MISSING_MAP_VIEW);
}

// --- audio: the audio system when started (M4), else the timing stub ---
void song_play(AssetKey song, uint16_t fade_in_frames)
{
   if (raw(song) == 0 && g_song_audio != 0)
   {
      audio_link::stop_song();
   }
   g_song_on = raw(song) != 0 ? 1 : 0;
   g_song_audio = g_song_on != 0 && audio_link::start_song(song, fade_in_frames) ? 1 : 0;
   g_song_start = g_clock;
   g_song_end = g_song_audio == 0 && g_song_length != 0 ? g_clock + g_song_length : 0;
   if (g_song_on && g_song_audio == 0)
   {
      note(presenter::MISSING_AUDIO);
   }
}

void song_fade(uint16_t frames)
{
   if (!g_song_on)
   {
      return;
   }
   if (g_song_audio != 0)
   {
      audio_link::fade_song(frames);
      return;
   }
   const uint32_t end = g_clock + frames;
   if (g_song_end == 0 || end < g_song_end)
   {
      g_song_end = end;
   }
}

bool song_playing()
{
   if (g_song_audio != 0)
   {
      // The frame it started in counts (the audio system steps at the next vertical blank).
      return g_song_on && (g_clock == g_song_start || audio_link::song_sounding());
   }
   return g_song_on && (g_song_end == 0 || g_clock < g_song_end);
}

uint32_t song_frame()
{
   return song_playing() ? g_clock - g_song_start : 0;
}

void sfx_play(AssetKey sound)
{
   if (!audio_link::play_sound(sound))
   {
      note(presenter::MISSING_AUDIO);
   }
}

} // namespace port
} // namespace capsule::seq

/// The link's defaults for a program without the audio system (the audio system's own definitions replace them:
/// src/audio/audio.cpp). They report no audio, so the port keeps its timing stub.
namespace capsule::seq::audio_link
{

CAPSULE_WEAK bool start_song(AssetKey, uint16_t)
{
   return false;
}

CAPSULE_WEAK void stop_song()
{
}

CAPSULE_WEAK bool fade_song(uint16_t)
{
   return false;
}

CAPSULE_WEAK bool song_sounding()
{
   return false;
}

CAPSULE_WEAK bool play_sound(AssetKey)
{
   return false;
}

} // namespace capsule::seq::audio_link
