/// @file
/// The overworld runner (see capsule/overworld/overworld.h) and the
/// overworld side of the script VM (capsule::script::world).
#include "capsule/overworld/overworld.h"

#include "capsule/core/assert.h"
#include "capsule/core/memory.h"
#include "capsule/encounter/encounter.h"
#include "capsule/field/doors.h"
#include "capsule/script/field_actions.h"
#include "capsule/script/field_effects.h"
#include "capsule/script/step_effects.h"
#include "capsule/gfx/background.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/input.h"
#include "capsule/hal/video.h"
#include "capsule/map/map.h"
#include "capsule/overworld/locations.h"
#include "capsule/script/port.h"
#include "capsule/script/script.h"
#include "capsule/seq/presenter.h"
#include "capsule/seq/sequence.h"
#include "capsule/text/placeholder.h"
#include "capsule/text/text_layer.h"

namespace capsule::overworld
{
namespace
{

namespace video = hal::video;
using script::Transition;

constexpr uint16_t kNoMap = 0xFFFF;              ///< shown_map(): no map shown.
constexpr unsigned kAllBanks = 0xFFFFFFFFu;      ///< Palette fade mask of every BG and OBJ bank.
constexpr uint8_t kNoFade = 0xFF;                ///< State::fade_coef before any fade was set.
constexpr int kByteOfBitShift = 3;               ///< Visited bits: location to its byte (loc / 8).
constexpr int kBitInByteMask = 7;                ///< Visited bits: location's bit in its byte (loc & 7).
constexpr int32_t kStepsMask = 0x7FFFFFFF;       ///< Query::Steps: the step count as a non-negative int32_t.

/// @name Banner
///@{
constexpr int kBannerPadCells = 4;      ///< Cells added to the name: the frame and a blank cell on each side.
constexpr int kBannerMinCells = 8;      ///< Narrowest banner.
constexpr int kBannerMaxCells = 30;     ///< Widest banner (the screen's width in cells).
constexpr uint8_t kBannerRows = 3;      ///< Banner height, cells.
constexpr int kFrameCells = 2;          ///< Cells the frame takes from the banner's width.
constexpr uint8_t kBannerNone = 0;      ///< Banner::phase: no banner.
constexpr uint8_t kBannerIn = 1;        ///< Banner::phase: sliding in.
constexpr uint8_t kBannerShown = 2;     ///< Banner::phase: shown.
constexpr uint8_t kBannerOut = 3;       ///< Banner::phase: sliding out.
///@}

/// @name Transitions and screen effects
///@{
constexpr int kLevelCovered = gfx::palette::kFadeFull;   ///< show_transition() level of a covered screen.
constexpr int kMaxTint = gfx::palette::kFadeFull;   ///< Strongest tint (the full fade coefficient).
constexpr int kMaxMosaic = 15;                ///< Largest mosaic step (at kLevelCovered).
constexpr uint8_t kAllLayersNoEffects = 0x1F;   ///< Window mask: every layer, color effects off.
/// @name Darkness (F2; C8: each level's lit box and brightness are the world's map::World::darkness_levels)
///@{
constexpr uint8_t kLayerBg0 = 0x01;   ///< Blend mask bit of BG0 (the text layer under the map plan).
constexpr int kCellHalf = map::kMetatilePixels / kHalf;   ///< The walker's cell centre, pixels.
///@}
constexpr uint8_t kFullDarken = 16;           ///< set_blend_brightness(): darkens to black.
constexpr video::Color kBlack = video::rgb5(0, 0, 0);      ///< Fade target of Fade and Mosaic.
constexpr video::Color kWhite = video::rgb5(31, 31, 31);   ///< Fade target of White.
constexpr int kShakePhaseBit = 2;             ///< Frame counter bit that flips the shake's side (every 2 frames).
///@}

/// @name Countdown timer
///@{
constexpr int kSecondsPerMinute = 60;   ///< Seconds per minute.
constexpr int kDecimalBase = 10;        ///< Digits are decimal.
constexpr int kTimerCodes = 8;          ///< Codes of the drawn timer ("mm:ss" and kCodeEnd, with room).
constexpr uint8_t kTimerRightCol = 29;  ///< Column the timer ends at (right aligned).
///@}

/// What a warp transition is doing.
enum class Phase : uint8_t
{
   None,      ///< No transition.
   Out,       ///< Covering the old map.
   Preview,   ///< Playing the location preview (the map is hidden).
   In,        ///< Uncovering the new map.
   DoorOpen,  ///< DS1: the warp's door opens.
   DoorEnter, ///< DS1: the walker walks into it.
   DoorClose, ///< DS1: it closes behind the hidden walker.
   DoorExit,  ///< DS1: the walker walks out of the door it arrived at.
   DoorShut,  ///< DS1: that door closes.
};

/// DS1: a door cell's column when the warp has no door.
constexpr int16_t kNoDoorCell = -32768;

/// A warp transition.
struct Trans
{
   Phase phase;            ///< What it is doing.
   Transition kind;        ///< How it looks.
   uint8_t t;              ///< Frames into the half.
   uint8_t facing;         ///< Facing on arrival (script::kKeepFacing: as now).
   uint16_t map;           ///< Destination map index.
   int16_t x;              ///< Landing column.
   int16_t y;              ///< Landing row.
   uint8_t banner;         ///< Location whose banner shows when it ends, or map::kNoLocation.
   hal::input::KeyBits held;   ///< Keys held on the preview's last frame (a press needs a release first).
   int16_t door_x;         ///< DS1: the door it leaves through (its anchor), or kNoDoorCell.
   int16_t door_y;         ///< Its row.
   bool exit_door;         ///< DS1: the landing cell is a door (the walker walks out once the map shows).
};

/// The name banner.
struct Banner
{
   uint8_t phase;          ///< 0 none, 1 sliding in, 2 shown, 3 sliding out.
   uint16_t t;             ///< Frames into the phase.
   uint8_t style;          ///< Banner style index.
   uint8_t rows;           ///< Height, cells.
};

/// Screen effects of scripts.
struct Screen
{
   video::Color tint;      ///< Tint color.
   uint8_t tint_from;      ///< Tint strength at the start of the change (0-16).
   uint8_t tint_to;        ///< Tint strength at its end.
   uint16_t tint_frames;   ///< Frames of the change.
   uint16_t tint_t;        ///< Frames into it.
   video::Color flash;     ///< Flash color.
   uint16_t flash_frames;  ///< Frames of the flash.
   uint16_t flash_t;       ///< Frames left of it.
   uint8_t shake;          ///< Shake power, pixels.
   uint16_t shake_t;       ///< Frames left of the shake.
   int32_t pan_x;          ///< Camera pan offset, pixels.
   int32_t pan_y;          ///< Its row offset.
   int32_t pan_tx;         ///< Where the pan goes.
   int32_t pan_ty;         ///< Its row.
   uint8_t pan_speed;      ///< 1-4.
};

/// Runner state.
struct State
{
   script::Context main;                         ///< The field script.
   script::Context parallel[kMaxParallel];       ///< Parallel object scripts.
   uint8_t parallel_obj[kMaxParallel];           ///< Their objects (field::kNoObject: free).
   uint16_t parallel_script[kMaxParallel];       ///< Their scripts.
   Trans trans;                                  ///< The warp transition.
   Banner banner;                                ///< The name banner.
   Screen screen;                                ///< Screen effects.
   int32_t timer_frames;                         ///< Countdown frames left (0: stopped).
   int32_t timer_shown;                          ///< Seconds drawn, or -1.
   bool timer_on;                                ///< The timer runs.
   bool timer_drawn;                             ///< The timer's text is on BG0.
   uint32_t fade_mask;                           ///< Palette fade last set: banks.
   video::Color fade_color;                      ///< Its target.
   uint8_t fade_coef;                            ///< Its coefficient (kNoFade: none set yet).
   field::Source last;                           ///< The last request's source.
   uint8_t frame;                                ///< Frame counter.
   bool ready;                                   ///< reset() has run (it sets the fields that are not 0).
   uint32_t steps_seen;                          ///< The walker's steps the field effects counted (F1).
   uint8_t darkness;                             ///< F2: the shown map's darkness level (0: lit).
   bool dark_shown;                              ///< F2: the darkness's window and blend are set.
};
State g{};   ///< The runner.
/// The location preview's player (about 1 KB, used only while a preview
/// plays): EWRAM, as battle::presenter_view keeps its own.
CAPSULE_EWRAM_BSS seq::Player g_preview;

/// The location of a map.
/// @param map  map index
/// @return location index, or map::kNoLocation
uint8_t location_of(uint16_t map)
{
   if (map >= map::kWorld.map_count)
   {
      return map::kNoLocation;
   }
   const uint8_t loc = map::kWorld.maps[map].location;
   return loc < kLocations.location_count ? loc : map::kNoLocation;
}

/// The shown map's index.
/// @return it, or 0xFFFF without a map
uint16_t shown_map()
{
   const map::Map* m = map::current();
   return m != nullptr ? map::index_of(*m) : kNoMap;
}

/// Whether a location was visited.
/// @param loc  location
/// @return true once marked
bool visited(uint8_t loc)
{
   return kLocations.visited != nullptr &&
          ((kLocations.visited[loc >> kByteOfBitShift] >> (loc & kBitInByteMask)) & 1) != 0;
}

/// Marks a location visited.
/// @param loc  location
void mark_visited(uint8_t loc)
{
   if (kLocations.visited != nullptr)
   {
      kLocations.visited[loc >> kByteOfBitShift] =
         static_cast<uint8_t>(kLocations.visited[loc >> kByteOfBitShift] | (1u << (loc & kBitInByteMask)));
   }
}

/// The preview to play on entering a location, if any.
/// @param loc  location
/// @return the sequence, or nullptr
const seq::Sequence* preview_of(uint8_t loc)
{
   const Location& l = kLocations.locations[loc];
   if (l.preview_count == 0 || (l.rule == PreviewRule::FirstVisit && visited(loc)))
   {
      return nullptr;
   }
   const uint32_t cond = encounter::conditions();
   for (int k = l.preview_count; k > 0; --k)
   {   // the last that holds wins
      const Preview& p = kLocations.previews[l.preview_first + k - 1];
      if ((p.conditions & cond) == p.conditions)
      {
         return p.sequence;
      }
   }
   return nullptr;
}

/// @name Banner
///@{

/// Closes the banner at once.
void hide_banner()
{
   if (g.banner.phase != kBannerNone)
   {
      text::layer::close_banner();
   }
   g.banner = Banner{};
   gfx::background::set_position(0, 0, 0);
   g.timer_shown = -1;   // redraw the timer
}

/// Shows a location's banner (sliding in).
/// @param loc  location
void show_banner(uint8_t loc)
{
   hide_banner();
   if (loc >= kLocations.location_count)
   {
      return;
   }
   const Location& l = kLocations.locations[loc];
   if (l.banner == kNoBanner || l.banner >= kLocations.style_count)
   {
      return;
   }
   if (gfx::background::is_reserved(0))
   {
      return;
   }   // the map uses BG0: no banner
   const BannerStyle& s = kLocations.styles[l.banner];
   // Cells, not codes: a field placeholder ({player}, D-22 decision 6) takes its text's width.
   const int len = text::line_cells(l.name);
   // The name and its padding, kept between the narrowest and widest banner.
   const auto w =
      static_cast<uint8_t>(len + kBannerPadCells > kBannerMaxCells
                              ? kBannerMaxCells
                              : (len + kBannerPadCells < kBannerMinCells ? kBannerMinCells : len + kBannerPadCells));
   g.banner.rows = kBannerRows;
   text::layer::open_banner(0, 0, w, g.banner.rows, s.frame, s.colors);
   text::layer::print(text::layer::kBannerTarget, static_cast<uint8_t>((w - kFrameCells) / kHalf), 0,
                      text::layer::Align::Center, false, l.name);
   g.banner.phase = kBannerIn;
   g.banner.style = l.banner;
   g.banner.t = 0;
   gfx::background::set_position(0, 0, static_cast<int16_t>(g.banner.rows * kTilePixels));   // starts above the screen
}

/// Advances the banner.
void update_banner()
{
   if (g.banner.phase == kBannerNone)
   {
      return;
   }
   if (!text::layer::banner_open())
   {   // a message box closed it (text::layer::open_box())
      g.banner = Banner{};
      gfx::background::set_position(0, 0, 0);
      g.timer_shown = -1;
      return;
   }
   const BannerStyle& s = kLocations.styles[g.banner.style];
   const int h = g.banner.rows * kTilePixels;
   ++g.banner.t;
   if (g.banner.phase == kBannerIn)
   {
      gfx::background::set_position(0, 0, static_cast<int16_t>(h - h * g.banner.t / s.slide));
      if (g.banner.t >= s.slide)
      {
         g.banner.phase = kBannerShown;
         g.banner.t = 0;
      }
   }
   else if (g.banner.phase == kBannerShown)
   {
      if (g.banner.t >= s.frames)
      {
         g.banner.phase = kBannerOut;
         g.banner.t = 0;
      }
   }
   else
   {
      gfx::background::set_position(0, 0, static_cast<int16_t>(h * g.banner.t / s.slide));
      if (g.banner.t >= s.slide)
      {
         hide_banner();
      }
   }
}

///@}

/// @name Screen effects
///@{

/// Banks the screen effects fade: every bank but the text styles' (from
/// text::layer::kFirstTextBank, or the map plan's first UI bank when that is
/// higher: banks below it are the map's), so text keeps its colors, as RMXP's
/// screen tone leaves its windows.
/// @return bank mask (bits 0-15 BG, 16-31 OBJ)
uint32_t effect_mask()
{
   const uint8_t ui = gfx::background::first_ui_bank();
   const uint8_t first = ui > text::layer::kFirstTextBank ? ui : text::layer::kFirstTextBank;
   uint32_t mask = kAllBanks;
   for (uint8_t b = first; b < gfx::palette::kBanks; ++b)
   {
      mask &= ~(1u << b);
   }
   return mask;
}

/// Sets the palette fade when it changes (a fade rewrites all 512 colors).
/// @param mask   banks
/// @param color  target
/// @param coef   0-16
void apply_fade(uint32_t mask, video::Color color, uint8_t coef)
{
   if (coef == 0)
   {   // no fade: one state, whatever the mask and color
      mask = 0;
      color = 0;
   }
   if (g.fade_coef == coef && g.fade_mask == mask && g.fade_color == color)
   {
      return;
   }
   g.fade_mask = mask;
   g.fade_color = color;
   g.fade_coef = coef;
   gfx::palette::set_fade(mask == 0 ? kAllBanks : mask, color, coef);
}

/// The palette fade the screen effects ask for.
/// @param[out] color  target color
/// @return coefficient 0-16
uint8_t effect_fade(video::Color& color)
{
   const Screen& s = g.screen;
   if (s.flash_t > 0)
   {
      color = s.flash;
      return static_cast<uint8_t>(gfx::palette::kFadeFull * s.flash_t / (s.flash_frames == 0 ? 1 : s.flash_frames));
   }
   color = s.tint;
   if (s.tint_frames == 0 || s.tint_t >= s.tint_frames)
   {
      return s.tint_to;
   }
   return static_cast<uint8_t>(s.tint_from + (int{s.tint_to} - s.tint_from) * s.tint_t / s.tint_frames);
}

/// Shows a transition at a level (0 = clear, 16 = covered).
/// @param kind   the transition
/// @param level  0-16
/// @param out    covering (else uncovering)
void show_transition(Transition kind, int level, bool out)
{
   video::Color fx = kBlack;
   uint8_t coef = effect_fade(fx);
   // Screen effects (tint, flash) leave the text banks alone; fades to black
   // or white cover everything.
   uint32_t mask = effect_mask();
   video::set_mosaic(0);
   video::set_windows(false, false, false);
   video::set_blend(video::BlendMode::Off, 0, 0);
   switch (kind)
   {
   case Transition::Fade:
   case Transition::White:
   case Transition::Mosaic:
      fx = kind == Transition::White ? kWhite : kBlack;
      coef = static_cast<uint8_t>(level);
      mask = kAllBanks;
      if (kind == Transition::Mosaic)
      {
         video::set_mosaic(static_cast<uint8_t>(1 + level * kMaxMosaic / kLevelCovered));
      }
      break;
   case Transition::WipeH:
   case Transition::WipeV:
   case Transition::Box:
   {
      int x0 = 0;
      int y0 = 0;
      int x1 = video::kScreenWidth;
      int y1 = video::kScreenHeight;
      if (kind == Transition::Box)
      {
         // A centred box that shrinks to nothing as the level rises.
         const int hw = video::kScreenWidth / kHalf * (kLevelCovered - level) / kLevelCovered;
         const int hh = video::kScreenHeight / kHalf * (kLevelCovered - level) / kLevelCovered;
         x0 = video::kScreenWidth / kHalf - hw;
         x1 = video::kScreenWidth / kHalf + hw;
         y0 = video::kScreenHeight / kHalf - hh;
         y1 = video::kScreenHeight / kHalf + hh;
      }
      else if (kind == Transition::WipeH)
      {
         if (out)
         {
            x0 = video::kScreenWidth * level / kLevelCovered;
         }
         else
         {
            x1 = video::kScreenWidth * (kLevelCovered - level) / kLevelCovered;
         }
      }
      else
      {
         if (out)
         {
            y0 = video::kScreenHeight * level / kLevelCovered;
         }
         else
         {
            y1 = video::kScreenHeight * (kLevelCovered - level) / kLevelCovered;
         }
      }
      if (level > 0)
      {
         // Inside window 0 the screen shows as is; outside, darkened to black.
         video::set_window_rect(0, static_cast<uint8_t>(x0), static_cast<uint8_t>(y0), static_cast<uint8_t>(x1),
                                static_cast<uint8_t>(y1));
         video::set_window_masks(kAllLayersNoEffects, kAllLayersNoEffects, video::kLayerAll, video::kLayerAll);
         video::set_windows(true, false, false);
         video::set_blend(video::BlendMode::Darken, video::kLayerAll, 0);
         video::set_blend_brightness(kFullDarken);
      }
      break;
   }
   default:
      break;
   }
   apply_fade(mask, fx, coef);
}

/// Advances the screen effects (outside transitions).
void update_screen()
{
   Screen& s = g.screen;
   if (s.tint_t < s.tint_frames)
   {
      ++s.tint_t;
   }
   if (s.flash_t > 0)
   {
      --s.flash_t;
   }
   video::Color c = 0;
   const uint8_t coef = effect_fade(c);
   apply_fade(effect_mask(), c, coef);
   // Camera pan towards its target, then the shake on top.
   // Pan speeds are route speed levels (route::kSpeedHalf moves on odd frames).
   const int step =
      s.pan_speed == route::kSpeedHalf
         ? (g.frame & 1)
         : (s.pan_speed == route::kSpeedDouble ? route::kSpeedDoublePixels
                                               : (s.pan_speed == route::kMaxSpeed ? route::kMaxSpeedPixels : 1));
   auto approach = [step](int32_t& v, int32_t to)
   {
      if (v < to)
      {
         v = v + step > to ? to : v + step;
      }
      else if (v > to)
      {
         v = v - step < to ? to : v - step;
      }
   };
   approach(s.pan_x, s.pan_tx);
   approach(s.pan_y, s.pan_ty);
   int32_t shake = 0;
   if (s.shake_t > 0)
   {
      --s.shake_t;
      shake = (g.frame & kShakePhaseBit) != 0 ? s.shake : -int32_t{s.shake};
      if (s.shake_t == 0)
      {
         shake = 0;
      }
   }
   field::set_camera_offset(s.pan_x + shake, s.pan_y);
}

/// Whether the timer may draw on BG0 now: the map leaves BG0 free, and no
/// banner, text box or typing uses it (printing would erase or finish them).
/// @return true when it may
bool timer_may_draw()
{
   return g.banner.phase == kBannerNone && !gfx::background::is_reserved(0) && gfx::background::map_plan() &&
          !text::layer::box_open() && !text::layer::busy();
}

/// Counts the timer down; it keeps counting through transitions and previews.
void tick_timer()
{
   if (g.timer_on && g.timer_frames > 0)
   {
      --g.timer_frames;
   }
}

/// Draws the countdown timer (m:ss) at the top right of BG0 when its seconds
/// change, or clears it once stopped; both wait while BG0 is busy.
void update_timer()
{
   if (!timer_may_draw())
   {
      return;
   }
   if (!g.timer_on)
   {
      if (g.timer_drawn)
      {
         text::layer::clear(0, 0, 1);
         g.timer_drawn = false;
      }
      return;
   }
   const int32_t secs = (g.timer_frames + kFramesPerSecond - 1) / kFramesPerSecond;   // rounded up
   if (secs == g.timer_shown)
   {
      return;
   }
   g.timer_shown = secs;
   const int32_t m = secs / kSecondsPerMinute;
   const int32_t sec = secs % kSecondsPerMinute;
   text::Code codes[kTimerCodes] = {};
   int n = 0;
   if (m >= kDecimalBase)
   {
      codes[n++] = static_cast<text::Code>('0' + m / kDecimalBase);
   }
   codes[n++] = static_cast<text::Code>('0' + m % kDecimalBase);
   codes[n++] = ':';
   codes[n++] = static_cast<text::Code>('0' + sec / kDecimalBase);
   codes[n++] = static_cast<text::Code>('0' + sec % kDecimalBase);
   codes[n] = text::kCodeEnd;
   text::layer::clear(0, 0, 1);
   text::layer::print(0, kTimerRightCol, 0, text::layer::Align::Right, false, codes);
   g.timer_drawn = true;
}

///@}

/// @name Transitions
///@{

/// F2: shows the darkness level: a lit box (window 0) around the walker, the map's layers and the sprites darkened
/// outside it (the Darken blend; BG0 stays bright when it is the text layer). Level 0 turns both off (once).
/// Register writes only, on every target (the soft video model has windows and blends); not during transitions,
/// which set their own.
void apply_darkness()
{
   if (g.darkness == 0 || g.darkness > map::kWorld.darkness_level_count || map::kWorld.darkness_levels == nullptr)
   {
      if (g.dark_shown)
      {
         video::set_windows(false, false, false);
         video::set_blend(video::BlendMode::Off, 0, 0);
         g.dark_shown = false;
      }
      return;
   }
   const field::Walker& w = field::walker();
   const int cx = static_cast<int>(w.px - map::camera_x()) + kCellHalf;
   const int cy = static_cast<int>(w.py - map::camera_y()) + kCellHalf;
   const map::DarknessLevel& level = map::kWorld.darkness_levels[g.darkness - 1];   // level n is entry n - 1
   const int half = level.lit_half;
   const auto clip = [](int v, int hi) { return static_cast<uint8_t>(v < 0 ? 0 : (v > hi ? hi : v)); };
   video::set_window_rect(0, clip(cx - half, video::kScreenWidth), clip(cy - half, video::kScreenHeight),
                          clip(cx + half, video::kScreenWidth), clip(cy + half, video::kScreenHeight));
   video::set_window_masks(kAllLayersNoEffects, kAllLayersNoEffects, video::kLayerAll, video::kLayerAll);
   video::set_windows(true, false, false);
   const uint8_t first = gfx::background::is_reserved(0) ? video::kLayerAll
                                                          : static_cast<uint8_t>(video::kLayerAll & ~kLayerBg0);
   video::set_blend(video::BlendMode::Darken, first, 0);
   video::set_blend_brightness(level.brightness);
   g.dark_shown = true;
}

/// F2: the darkness level a map starts with: its own, or (C8) the level a script set on it, kept in the bank's
/// map-lifetime darkness variable (cleared when the walker goes to another map; kept by a save, so a loaded game
/// starts as dark as it was saved; also what the map's on_transition / on_load scripts set while it loaded).
/// @param map  map index
void enter_darkness(uint16_t map)
{
   uint8_t level = map < map::kWorld.map_count ? map::kWorld.maps[map].darkness : 0;
   const uint16_t kept = script::kBank.darkness_var;
   if (kept != script::kNoVar && script::var(script::VarId{kept}) > 0)
   {
      level = static_cast<uint8_t>(script::var(script::VarId{kept}) - 1);   // the variable holds level + 1
   }
   const uint8_t darkest = map::kWorld.darkness_level_count;
   g.darkness = level > darkest ? darkest : level;
   g.dark_shown = false;   // the transition left windows and blend as it wanted
}

/// Shows the destination map (the field enters it) and starts uncovering.
void arrive()
{
   Trans& t = g.trans;
   const uint8_t facing = t.facing < field::NUM_DIRECTIONS ? t.facing : field::walker().facing;
   field::enter(t.map, t.x, t.y, facing);
   enter_darkness(t.map);
   field::reload_walker_sprite();
   // DS1: arriving on a door: it shows open, the walker hidden behind it until the map shows
   t.exit_door = field::doors::set_open(t.x, t.y);
   if (t.exit_door)
   {
      field::show_walker(false);
   }
   t.phase = t.kind == Transition::None ? Phase::None : Phase::In;
   t.t = kTransitionFrames;
   if (t.phase == Phase::In)
   {
      show_transition(t.kind, kLevelCovered, false);
   }
}

/// The old map is covered: preview or arrive.
void swap()
{
   Trans& t = g.trans;
   const uint8_t from = location_of(shown_map());
   const uint8_t to = location_of(t.map);
   t.banner = map::kNoLocation;
   const seq::Sequence* pv = nullptr;
   if (to != map::kNoLocation && to != from)
   {
      t.banner = to;
      pv = preview_of(to);
      mark_visited(to);
   }
   if (pv != nullptr)
   {
      field::leave();   // the map and its objects go; the next enter() still clears its map flags
      video::set_mosaic(0);
      video::set_windows(false, false, false);
      seq::presenter::reset();
      t.held = hal::input::held();   // a key still held from the warp is not a press
      seq::start(g_preview, *pv);
      t.phase = Phase::Preview;
      return;
   }
   arrive();
}

/// Ends the transition (and shows the banner when the location changed).
void finish()
{
   Trans& t = g.trans;
   show_transition(Transition::None, 0, false);
   apply_darkness();   // F2: no lit frame between the transition and the dark map
   const asset::Door* d = t.exit_door ? field::doors::at(t.x, t.y) : nullptr;
   t.exit_door = false;
   if (d != nullptr)
   {   // DS1: the walker shows and walks out of the door it arrived at; the door closes after it
      field::show_walker(true);
      field::move_walker(d->exit, 1);
      t.phase = Phase::DoorExit;
      return;
   }
   t.phase = Phase::None;
   if (t.banner != map::kNoLocation)
   {
      show_banner(t.banner);
   }
}

/// DS1: the direction from the walker's cell to a neighbouring cell.
/// @param x  the cell's column
/// @param y  its row
/// @return a field::Direction, or field::kNoDirection (the walker stands on it, or it is not a neighbour)
uint8_t toward(int x, int y)
{
   const field::Walker& w = field::walker();
   for (uint8_t d = 0; d < field::NUM_DIRECTIONS; ++d)
   {
      if (w.x + field::dx_of(d) == x && w.y + field::dy_of(d) == y)
      {
         return d;
      }
   }
   return field::kNoDirection;
}

/// DS1: one frame of a door phase of the warp (the field runs on: the doors animate in field::update()).
void update_door()
{
   Trans& t = g.trans;
   field::update(field::Intent{});
   switch (t.phase)
   {
   case Phase::DoorOpen:
      if (!field::doors::busy())
      {
         const uint8_t d = toward(t.door_x, t.door_y);
         if (d != field::kNoDirection)
         {
            field::move_walker(d, 1);
         }
         t.phase = Phase::DoorEnter;
      }
      break;
   case Phase::DoorEnter:
      if (!field::walker_moving())
      {
         field::show_walker(false);
         field::doors::close(t.door_x, t.door_y);
         t.phase = Phase::DoorClose;
      }
      break;
   case Phase::DoorClose:
      if (!field::doors::busy())
      {
         t.door_x = kNoDoorCell;
         t.phase = Phase::Out;
         if (t.kind == Transition::None)
         {
            swap();
            if (t.phase == Phase::None)
            {
               finish();
            }
         }
      }
      break;
   case Phase::DoorExit:
      if (!field::walker_moving())
      {
         field::doors::close(t.x, t.y);
         t.phase = Phase::DoorShut;
      }
      break;
   case Phase::DoorShut:
      if (!field::doors::busy())
      {
         t.phase = Phase::None;
         if (t.banner != map::kNoLocation)
         {
            show_banner(t.banner);
         }
      }
      break;
   default:
      break;
   }
}

/// Advances the transition one frame.
void update_transition()
{
   Trans& t = g.trans;
   switch (t.phase)
   {
   case Phase::Out:
      ++t.t;
      show_transition(t.kind, kLevelCovered * t.t / kTransitionFrames, true);
      if (t.t >= kTransitionFrames)
      {
         swap();
      }
      break;
   case Phase::Preview:
   {
      const hal::input::KeyBits now = hal::input::held();
      const auto pressed = static_cast<hal::input::KeyBits>(now & ~t.held);
      t.held = now;
      const seq::Status st = seq::step(g_preview, now, pressed);
      seq::presenter::frame();
      if (st == seq::Status::Exited)
      {
         seq::presenter::reset();
         text::layer::reset();
         g.fade_coef = kNoFade;   // the preview set fades of its own
         arrive();
         if (t.phase == Phase::None)
         {
            finish();
         }
      }
      break;
   }
   case Phase::In:
      if (t.t > 0)
      {
         --t.t;
      }
      show_transition(t.kind, kLevelCovered * t.t / kTransitionFrames, false);
      if (t.t == 0)
      {
         finish();
      }
      break;
   case Phase::DoorOpen:
   case Phase::DoorEnter:
   case Phase::DoorClose:
   case Phase::DoorExit:
   case Phase::DoorShut:
      update_door();
      break;
   case Phase::None:
      break;
   }
}

///@}

/// Starts the main field script.
/// @param s     script ID
/// @param self  its object, or field::kNoObject
void run_main(uint16_t s, uint8_t self)
{
   hide_banner();   // the original games close the name popup when a script starts
   script::start(g.main, script::ScriptId{s}, self);
}

/// F1: runs the timed field effects down by the walker's new steps and the minute clock
/// (capsule/script/field_effects.h); F3: counts the same steps into the step effects (capsule/script/step_effects.h:
/// the game's battle side runs the ones that are due).
void count_field_effects()
{
   const uint32_t steps = field::walker().steps;
   // the walker's count only grows; anything else (a new game's walker) counts nothing
   const uint32_t walked = steps >= g.steps_seen ? steps - g.steps_seen : 0;
   script::field_effects::walked(walked);
   script::step_effects::walked(walked);
   g.steps_seen = steps;
   script::field_effects::tick();
}

/// F1: the expiry script of the next timed field effect that ended (effects without a message end quietly).
/// @param[out] out  its ScriptId
/// @return false when none waits
bool expired_message(uint16_t& out)
{
   uint8_t fx = 0;
   while (script::field_effects::take_expired(&fx))
   {
      const script::FieldEffectInfo* info = script::field_effects::info(fx);
      if (info != nullptr && info->expiry_script != script::kNoExpiryScript)
      {
         out = info->expiry_script;
         return true;
      }
   }
   return false;
}

/// Keeps the parallel contexts in step with the Parallel objects' pages.
void update_parallel()
{
   const uint8_t n = field::object_count();
   for (int k = 0; k < kMaxParallel; ++k)
   {   // stop those whose page changed
      if (g.parallel_obj[k] == field::kNoObject)
      {
         continue;
      }
      const uint8_t obj = g.parallel_obj[k];
      if (obj >= n || field::parallel_script(obj) != g.parallel_script[k])
      {
         script::stop(g.parallel[k]);
         g.parallel_obj[k] = field::kNoObject;
      }
   }
   for (uint8_t i = 0; i < n; ++i)
   {
      const uint16_t s = field::parallel_script(i);
      if (s == field::kNoScript)
      {
         continue;
      }
      int slot = -1;
      int free = -1;
      for (int k = 0; k < kMaxParallel; ++k)
      {
         if (g.parallel_obj[k] == i)
         {
            slot = k;
         }
         if (g.parallel_obj[k] == field::kNoObject && free < 0)
         {
            free = k;
         }
      }
      if (slot < 0)
      {
         ENGINE_ASSERT(free >= 0 && "more Parallel objects than kMaxParallel");
         if (free < 0)
         {
            continue;
         }
         slot = free;
         g.parallel_obj[slot] = i;
         g.parallel_script[slot] = s;
      }
      if (!script::running(g.parallel[slot]))
      {
         script::start(g.parallel[slot], script::ScriptId{s}, i);
      }
      script::update(g.parallel[slot]);
   }
}

/// Clears the parallel contexts.
void clear_parallel()
{
   for (int k = 0; k < kMaxParallel; ++k)
   {
      script::stop(g.parallel[k]);
      g.parallel_obj[k] = field::kNoObject;
   }
}

/// Starts a transition.
/// F2: a warp from a map with an escape point to one without records it: the cell one step back from the walker
/// (where it came from), facing away from the warp.
/// @param to  the destination map
void record_escape(uint16_t to)
{
   const uint16_t from = shown_map();
   if (from >= map::kWorld.map_count || to >= map::kWorld.map_count ||
       (map::kWorld.maps[from].flags & map::kMapEscapePoint) == 0 ||
       (map::kWorld.maps[to].flags & map::kMapEscapePoint) != 0)
   {
      return;
   }
   const field::Walker& w = field::walker();
   const uint8_t back = w.facing == field::DIR_DOWN ? field::DIR_UP
                        : w.facing == field::DIR_UP  ? field::DIR_DOWN
                        : w.facing == field::DIR_LEFT ? field::DIR_RIGHT
                                                      : field::DIR_LEFT;
   script::actions::record_escape(map::key_of(from), static_cast<int16_t>(w.x + field::dx_of(back)),
                                  static_cast<int16_t>(w.y + field::dy_of(back)), back);
}

/// @param map         destination map
/// @param x           column
/// @param y           row
/// @param facing      facing, or kKeepFacing
/// @param transition  Transition, or kTransitionDefault
/// @param door_x      DS1: the cell of the warp the walker takes (a door there opens first), or kNoDoorCell
/// @param door_y      its row
void begin(uint16_t map, int x, int y, uint8_t facing, uint8_t transition, int16_t door_x = kNoDoorCell,
           int16_t door_y = 0)
{
   ENGINE_ASSERT(map < map::kWorld.map_count);
   if (map >= map::kWorld.map_count)
   {
      return;
   }
   hide_banner();
   clear_parallel();
   g.main.self = script::kNoSelf;   // the old map's local IDs mean nothing on the new one
   record_escape(map);
   Trans& t = g.trans;
   uint8_t kind = transition == script::kTransitionDefault ? kLocations.default_transition : transition;
   if (kind >= static_cast<uint8_t>(Transition::NUM_TRANSITIONS))
   {
      kind = static_cast<uint8_t>(Transition::Fade);
   }
   t.kind = static_cast<Transition>(kind);
   t.map = map;
   t.x = static_cast<int16_t>(x);
   t.y = static_cast<int16_t>(y);
   t.facing = facing;
   t.t = 0;
   t.exit_door = false;
   t.door_x = kNoDoorCell;
   g.screen.pan_x = g.screen.pan_tx = 0;
   g.screen.pan_y = g.screen.pan_ty = 0;
   if (door_x != kNoDoorCell && field::doors::open(door_x, door_y))
   {   // DS1: the door opens, the walker walks in, it closes; then the transition
      t.door_x = door_x;
      t.door_y = door_y;
      t.phase = Phase::DoorOpen;
      return;
   }
   if (t.kind == Transition::None)
   {
      swap();
      if (t.phase == Phase::None)
      {
         finish();
      }
      return;
   }
   t.phase = Phase::Out;
}

}   // namespace

void start(uint16_t map, int x, int y, uint8_t facing)
{
   reset();
   // C8: the darkness a script set (kept by a save) belongs to the map play starts on: entering it from a shown map
   // clears the map lifetime, so it is put back unless the map's own scripts set another one while it loaded
   const uint16_t dark_var = script::kBank.darkness_var;
   const int32_t kept = dark_var != script::kNoVar ? script::var(script::VarId{dark_var}) : 0;
   field::enter(map, x, y, facing);
   if (kept > 0 && script::var(script::VarId{dark_var}) == 0)
   {
      script::set_var(script::VarId{dark_var}, kept);
   }
   enter_darkness(map);
   const uint8_t loc = location_of(map);
   if (loc != map::kNoLocation)
   {
      mark_visited(loc);
      show_banner(loc);
   }
}

void reset()
{
   hide_banner();
   clear_parallel();
   if (script::running(g.main))
   {
      field::release();
   }
   script::stop(g.main);
   if (g.timer_drawn && !gfx::background::is_reserved(0) && gfx::background::map_plan())
   {
      text::layer::clear(0, 0, 1);
   }
   g.timer_drawn = false;
   g.fade_coef = kNoFade;   // the fade below is set for certain
   g.ready = true;
   g.trans.phase = Phase::None;
   g.screen = Screen{};
   g.screen.pan_speed = route::kDefaultSpeed;
   g.timer_on = false;
   g.timer_frames = 0;
   g.timer_shown = -1;
   g.last = field::Source::NUM_SOURCES;
   video::set_mosaic(0);
   video::set_windows(false, false, false);
   video::set_blend(video::BlendMode::Off, 0, 0);
   field::set_camera_offset(0, 0);
   apply_fade(kAllBanks, 0, 0);
   // F1: the timed field effects count from here (the walker's steps so far, the minute clock read again; C8: an
   // expiry not reported yet is forgotten); F3 / C8: so are the step effects due but not run yet (their counters
   // stay: they are the bank's variables)
   g.steps_seen = field::walker().steps;
   script::field_effects::reset_clock();
   script::step_effects::clear_due();
}

void update(const field::Intent& intent)
{
   if (!g.ready)
   {
      reset();
   }   // update() before start()
   ++g.frame;
   script::frame();
   tick_timer();
   if (g.trans.phase != Phase::None)
   {
      update_transition();
      if (g.trans.phase != Phase::Preview)
      {
         text::layer::update();
      }
      return;
   }
   if (script::running(g.main))
   {
      script::update(g.main);
      if (g.trans.phase == Phase::None)
      {
         field::update(field::Intent{});   // the walker stands; objects' scripted walks go on
         if (!script::running(g.main))
         {
            field::release();
         }
      }
   }
   else
   {
      field::update(intent);
      field::FieldEvent e{};
      if (field::take_event(&e))
      {
         g.last = e.source;
         if (e.what == field::Request::Warp)
         {
            begin(e.map, e.x, e.y, script::kKeepFacing, e.transition, e.at_x, e.at_y);
         }
         else if (e.what == field::Request::Script)
         {
            run_main(e.script, e.object);
         }
      }
      else if (!field::has_event())
      {
         script::ScriptId fired{};
         uint16_t expiry = script::kNoExpiryScript;
         if (!field::locked() && expired_message(expiry))
         {
            field::lock();   // F1: a timed field effect ended: its expiry message, as a trigger's script runs
            run_main(expiry, field::kNoObject);
         }
         else if (!field::locked() && script::poll_triggers(fired))
         {
            field::lock();
            run_main(static_cast<uint16_t>(fired), field::kNoObject);
         }
      }
   }
   count_field_effects();
   if (g.trans.phase == Phase::None)
   {
      update_parallel();
      update_screen();
      apply_darkness();
      update_banner();
      update_timer();
   }
   text::layer::update();
}

void warp(uint16_t map, int x, int y, uint8_t facing, uint8_t transition)
{
   begin(map, x, y, facing, transition);
}

bool can_run_script()
{
   return !script::running(g.main) && g.trans.phase == Phase::None && !field::has_event();
}

bool run_script(uint16_t script, uint8_t self)
{
   if (!can_run_script())
   {
      return false;
   }
   field::lock();
   run_main(script, self);
   return true;
}

uint8_t darkness()
{
   return g.darkness;
}

bool script_running()
{
   return script::running(g.main);
}

bool transitioning()
{
   return g.trans.phase != Phase::None;
}

bool previewing()
{
   return g.trans.phase == Phase::Preview;
}

field::Source last_source()
{
   return g.last;
}

int32_t timer_seconds()
{
   return g.timer_on ? (g.timer_frames + kFramesPerSecond - 1) / kFramesPerSecond : 0;
}

bool banner_shown()
{
   return g.banner.phase != kBannerNone;
}

}   // namespace capsule::overworld

namespace capsule::script::world
{
namespace
{

/// The cell of an object or the walker.
/// @param obj    local ID, or kObjectPlayer
/// @param[out] x  column
/// @param[out] y  row
/// @return false when there is no such (shown) object
bool cell_of(uint8_t obj, int& x, int& y)
{
   if (obj == kObjectPlayer)
   {
      x = field::walker().x;
      y = field::walker().y;
      return true;
   }
   const field::Object* o = field::object(obj);
   if (o == nullptr || !o->active)
   {
      return false;
   }
   x = o->x;
   y = o->y;
   return true;
}

}   // namespace

bool move(uint8_t obj, uint8_t dir, uint16_t steps)
{
   if (obj == kObjectPlayer)
   {
      field::move_walker(dir, steps);
      return true;
   }
   return field::move_object(obj, dir, steps);
}

bool path_clear(uint8_t obj, uint8_t dir, uint16_t steps)
{
   return obj == kObjectPlayer ? field::walker_path_clear(dir, steps) : field::object_path_clear(obj, dir, steps);
}

bool moving(uint8_t obj)
{
   return obj == kObjectPlayer ? field::walker_moving() : field::object_moving(obj);
}

void face(uint8_t obj, uint8_t dir)
{
   if (obj == kObjectPlayer)
   {
      field::face_walker(dir);
   }
   else
   {
      field::face_object(obj, dir);
   }
}

void face_toward(uint8_t obj, uint8_t other)
{
   int x = 0;
   int y = 0;
   int ox = 0;
   int oy = 0;
   if (!cell_of(obj, x, y) || !cell_of(other, ox, oy) || (x == ox && y == oy))
   {
      return;
   }
   const int dx = ox - x;
   const int dy = oy - y;
   const int adx = dx < 0 ? -dx : dx;
   const int ady = dy < 0 ? -dy : dy;
   uint8_t d = field::DIR_DOWN;
   if (adx >= ady)
   {
      d = dx < 0 ? field::DIR_LEFT : field::DIR_RIGHT;
   }
   else
   {
      d = dy < 0 ? field::DIR_UP : field::DIR_DOWN;
   }
   face(obj, d);
}

void show(uint8_t obj, bool on)
{
   field::show_object(obj, on);
}

bool route(uint8_t obj, const uint8_t* code, uint16_t len, bool from_script)
{
   if (len < route::kMinRouteBytes)
   {
      return false;
   }
   if (obj == kObjectPlayer)
   {
      field::route_walker(code);
      return true;
   }
   return field::route_object(obj, code, from_script);
}

bool routes_busy()
{
   return field::routes_busy();
}

bool self_switch(uint8_t obj, uint8_t letter)
{
   return field::self_switch(obj, letter);
}

void set_self_switch(uint8_t obj, uint8_t letter, bool on)
{
   field::set_self_switch(obj, letter, on);
}

int32_t query(Query q, uint8_t obj)
{
   int x = 0;
   int y = 0;
   switch (q)
   {
   case Query::PosX:
   case Query::PosY:
      if (!cell_of(obj, x, y))
      {
         return 0;
      }
      return q == Query::PosX ? x : y;
   case Query::Facing:
      if (obj == kObjectPlayer)
      {
         return field::walker().facing;
      }
      return field::object(obj) != nullptr ? field::object(obj)->facing : 0;
   case Query::MapIndex:
   {
      const map::Map* m = map::current();
      return m != nullptr ? map::index_of(*m) : 0;
   }
   case Query::Steps:
      return static_cast<int32_t>(field::walker().steps & overworld::kStepsMask);
   case Query::Timer:
      return overworld::timer_seconds();
   case Query::NUM_QUERIES:
      break;
   }
   return 0;
}

// I4: weak (CAPSULE_WEAK), so a game that runs its own warp sequence (an imported game's doors and fades) keeps
// its own warp() and warping() and the rest of this port
CAPSULE_WEAK void warp(AssetKey map, int16_t x, int16_t y, uint8_t facing, uint8_t transition)
{
   const map::Map* m = map::find(map);
   ENGINE_ASSERT(m != nullptr && "warp to a map that is not in the world");
   if (m == nullptr)
   {
      return;
   }
   overworld::begin(map::index_of(*m), x, y, facing, transition);
}

CAPSULE_WEAK bool warping()
{
   return overworld::transitioning();
}

void set_darkness(uint8_t level)
{
   const uint8_t darkest = map::kWorld.darkness_level_count;
   overworld::g.darkness = level > darkest ? darkest : level;
   // C8: kept (level + 1) in the map-lifetime variable a save keeps; the next map clears it
   if (kBank.darkness_var != kNoVar)
   {
      set_var(VarId{kBank.darkness_var}, int32_t{overworld::g.darkness} + 1);
   }
}

bool escape(uint8_t transition)
{
   AssetKey to{};
   int16_t x = 0;
   int16_t y = 0;
   uint8_t facing = 0;
   if (!actions::escape_point(to, x, y, facing))
   {
      return false;
   }
   // C8: the recorded key resolves through the world; a map the build no longer has is no escape point
   const map::Map* m = map::find(to);
   if (m == nullptr)
   {
      return false;
   }
   overworld::begin(map::index_of(*m), x, y, facing, transition);
   return true;
}

void place(uint8_t obj, int16_t x, int16_t y, uint8_t facing)
{
   if (obj == kObjectPlayer)
   {
      field::place(x, y, facing < field::NUM_DIRECTIONS ? facing : static_cast<uint8_t>(field::walker().facing));
      return;
   }
   field::place_object(obj, x, y,
                       facing < field::NUM_DIRECTIONS ? facing : static_cast<uint8_t>(field::NUM_DIRECTIONS));
}

void scroll(uint8_t dir, int16_t cells, uint8_t speed)
{
   auto& s = overworld::g.screen;
   s.pan_speed = speed >= 1 && speed <= route::kMaxSpeed ? speed : route::kDefaultSpeed;
   if (dir >= field::NUM_DIRECTIONS)
   {
      s.pan_tx = 0;
      s.pan_ty = 0;
      return;
   }
   s.pan_tx += field::dx_of(dir) * int32_t{cells} * map::kMetatilePixels;
   s.pan_ty += field::dy_of(dir) * int32_t{cells} * map::kMetatilePixels;
}

bool scrolling()
{
   const auto& s = overworld::g.screen;
   return s.pan_x != s.pan_tx || s.pan_y != s.pan_ty;
}

void tint(hal::video::Color color, uint8_t strength, uint16_t frames)
{
   auto& s = overworld::g.screen;
   const int now = (s.tint_frames == 0 || s.tint_t >= s.tint_frames)
                      ? s.tint_to
                      : s.tint_from + (int{s.tint_to} - s.tint_from) * s.tint_t / s.tint_frames;
   s.tint_from = static_cast<uint8_t>(now);   // changes continue from the strength shown
   if (strength > 0)
   {
      s.tint = color;
   }   // `tint off` fades the shown color out
   s.tint_to = strength > overworld::kMaxTint ? uint8_t{overworld::kMaxTint} : strength;
   s.tint_frames = frames;
   s.tint_t = 0;
}

void flash(hal::video::Color color, uint16_t frames)
{
   auto& s = overworld::g.screen;
   s.flash = color;
   s.flash_frames = frames == 0 ? uint16_t{1} : frames;
   s.flash_t = s.flash_frames;
}

void shake(uint8_t power, uint16_t frames)
{
   auto& s = overworld::g.screen;
   s.shake = power;
   s.shake_t = frames;
}

void timer_start(int32_t seconds)
{
   auto& st = overworld::g;
   st.timer_on = true;
   st.timer_frames = seconds * kFramesPerSecond;
   st.timer_shown = -1;
}

void timer_stop()
{
   auto& st = overworld::g;
   st.timer_on = false;   // update_timer() clears its text once BG0 is free
   st.timer_frames = 0;
}

void encounters(bool on)
{
   field::set_encounters(on);
}

}   // namespace capsule::script::world
