/// @file
/// The scene kit's sandbox scenes (see scenes.h). Graphics are made by code: a 32x32 ball (body, ring, a top
/// marker so turns show), a 16x16 box, a striped and a checked background tile.
#include "scenes.h"

#include "capsule/anim/bank.h"
#include "capsule/anim/player.h"
#include "capsule/core/memory.h"
#include "capsule/core/trig.h"
#include "capsule/gfx/palette.h"
#include "capsule/hal/video.h"
#include "capsule/screen/affine.h"
#include "capsule/screen/blend.h"
#include "capsule/screen/fade.h"
#include "capsule/screen/layers.h"
#include "capsule/screen/scanline.h"
#include "capsule/screen/sprites.h"
#include "capsule/screen/tasks.h"

extern const uint8_t kShapesBank[];       ///< data/shapes.canim, assembled at build time.
extern const uint32_t kShapesBank_size;   ///< Its bytes.

namespace sandbox_anim
{
namespace
{

namespace video = capsule::hal::video;
namespace sp = capsule::screen::sprites;
namespace af = capsule::screen::affine;
namespace tasks = capsule::screen::tasks;
namespace layers = capsule::screen::layers;
namespace sl = capsule::screen::scanline;
namespace blend = capsule::screen::blend;
namespace fade = capsule::screen::fade;
namespace anim = capsule::anim;
namespace palette = capsule::gfx::palette;

/// @name Shapes
///@{
constexpr int kBallSide = 32;              ///< Ball sprite side.
constexpr int kBallTiles = 16;             ///< Its 4bpp tiles.
constexpr int kBoxSide = 16;               ///< Box sprite side.
constexpr int kBoxTiles = 4;               ///< Its tiles.
constexpr int kTileWords = 8;              ///< Words of a 4bpp tile.
constexpr int kTileSide = 8;               ///< Pixels per tile side.
constexpr int kNibbleBits = 4;             ///< Bits per 4bpp pixel.
constexpr int kBallRadius2 = 15 * 15;      ///< Ball body: inside this squared radius (from the centre).
constexpr int kRingRadius2 = 9 * 9;        ///< Ring: outside this.
constexpr int kMarkerRows = 6;             ///< Top rows of the marker.
constexpr int kMarkerHalfWidth = 3;        ///< Marker half width.
constexpr uint32_t kBody = 1;              ///< Ball body colour index.
constexpr uint32_t kRing = 2;              ///< Ring colour index.
constexpr uint32_t kMarker = 3;            ///< Marker colour index.
constexpr uint32_t kEdge = 4;              ///< Box edge colour index.
constexpr int kStripes = 8;                ///< BG stripe colours (indices 1-8).
constexpr uint32_t kCheckA = 9;            ///< Checker colour A.
constexpr uint32_t kCheckB = 10;           ///< Checker colour B.
constexpr int kCheckHalf = 4;              ///< Checker square side.
///@}

/// @name Tags, banks and layers
///@{
constexpr uint16_t kBallTag = 1;           ///< Ball sheet and palette tag.
constexpr uint16_t kBoxTag = 2;            ///< Box sheet tag.
constexpr uint16_t kRedTag = 2;            ///< Blend scene palette tags.
constexpr uint16_t kGreenTag = 3;          ///< ...
constexpr uint16_t kBlueTag = 4;           ///< ...
constexpr uint8_t kStripeScreenBlock = 30; ///< BG0's map.
constexpr uint8_t kCheckScreenBlock = 31;  ///< BG1's map.
constexpr int kMapCells = 32;              ///< Cells per map side.
constexpr uint16_t kStripeTile = 1;        ///< BG tile numbers.
constexpr uint16_t kCheckTile = 2;         ///< ...
constexpr uint8_t kSpritePriority = 1;     ///< The sprites' priority.
///@}

/// @name Timing and motion
///@{
constexpr int kPartFrames = 60;            ///< A scanline part's frames.
constexpr int kBlendPartFrames = 90;       ///< The blend scene's ramp part.
constexpr int kCentreX = 120;              ///< Screen centre.
constexpr int kCentreY = 80;               ///< ...
constexpr int kQuarterX = 60;              ///< Left quarter.
constexpr int kThreeQuarterX = 180;        ///< Right quarter.
constexpr int kTopY = 48;                  ///< Upper row.
constexpr int kBottomY = 112;              ///< Lower row.
constexpr int kWeightCycle = 17;           ///< Blend weights 0-16.
constexpr int kEvbShift = 8;               ///< BLDALPHA: evb's bits.
constexpr int kDiamondStart = 20;          ///< Diamond's first radius.
constexpr int kWaveAmplitude = 6;          ///< Wave amplitude.
constexpr uint8_t kWaveFrequency = 4;      ///< Wave frequency.
constexpr uint8_t kWaveDelay = 1;          ///< Wave delay.
constexpr int kSineToQ8 = 6;               ///< Q14 to 8.8: >> 6.
constexpr int kTurnStepShift = 9;          ///< The box's turn per frame: t << 9.
constexpr int kOrbitStep = 3;              ///< Orbit angle step (1/256 turns) per frame.
constexpr int kAngleShift = 8;             ///< 1/256 turns to a 16-bit turn.
constexpr int kTriangle = 32;              ///< Triangle wave period of the tag blend.
constexpr int kRestartFrames = 10;         ///< Frames between two script runs.
///@}

/// @name Colours
///@{
constexpr video::Color kBackdrop = video::rgb5(2, 4, 10);       ///< Backdrop.
constexpr video::Color kWhite = video::rgb5(31, 31, 31);        ///< White.
constexpr video::Color kBlack = 0;                              ///< Black.
constexpr video::Color kYellow = video::rgb5(31, 31, 0);        ///< The box's edge.
constexpr video::Color kCheckColorA = video::rgb5(6, 6, 14);    ///< Checker A.
constexpr video::Color kCheckColorB = video::rgb5(10, 10, 20);  ///< Checker B.
constexpr int kStripeRed = 3;      ///< Stripe i's red: i x 3.
constexpr int kStripeGreen = 24;   ///< Stripe i's green: 24 - i x 2.
constexpr int kStripeGreenStep = 2;   ///< ...
constexpr int kStripeBlue = 8;     ///< Stripe i's blue: 8 + i.
constexpr uint8_t kStrong = 28;    ///< A ball's main channel.
constexpr uint8_t kWeak = 6;       ///< Its other channels.
constexpr uint8_t kMid = 8;        ///< The affine scene's green.
constexpr uint8_t kGoldR = 24;     ///< The script's ball: red.
constexpr uint8_t kGoldG = 20;     ///< ... green.
constexpr uint8_t kGoldB = 4;      ///< ... blue.
///@}

/// @name Scenes and their parts
///@{
constexpr int kSceneAffine = 0;    ///< Scene numbers.
constexpr int kSceneScanline = 1;  ///< ...
constexpr int kSceneBlend = 2;     ///< ...
constexpr int kBgTileCount = 3;    ///< BG tiles: blank, stripes, checks.
constexpr int kSineEntries = 256;  ///< The wave's sine table.
constexpr int kHalfDivisor = 2;    ///< Halves.
constexpr uint8_t kBg0Bit = 1;     ///< BG0's layer mask bit.
constexpr uint8_t kBg1Bit = 2;     ///< BG1's layer mask bit.
constexpr uint8_t kStripePriority = 1;   ///< BG0's priority.
constexpr uint8_t kCheckPriority = 2;    ///< BG1's priority.
constexpr int kScriptSpriteSlots = 4;    ///< Sprites the script's hook keeps.
constexpr int kFadeTags = 3;             ///< Tags of the blend scene's fade.
///@}

CAPSULE_EWRAM_BSS uint32_t g_ball[kBallTiles * kTileWords];   ///< Ball tiles.
CAPSULE_EWRAM_BSS uint32_t g_box[kBoxTiles * kTileWords];     ///< Box tiles.
uint32_t g_bg_tiles[kBgTileCount * kTileWords];               ///< Blank, stripe and checker BG tiles.
CAPSULE_EWRAM_BSS int16_t g_sine[kSineEntries];               ///< The wave's 8.8 sine table.
bool g_made = false;                                          ///< Shapes made.
Report g_report{};                                            ///< The rounds' report.

/// Writes one 4bpp pixel into a sprite's tiles (1D: tiles row by row).
/// @param words  the tiles
/// @param side   sprite side in pixels
/// @param x      column
/// @param y      row
/// @param c      colour index
void put_pixel(uint32_t* words, int side, int x, int y, uint32_t c)
{
   const int tile = (y / kTileSide) * (side / kTileSide) + x / kTileSide;
   words[tile * kTileWords + y % kTileSide] |= c << ((x % kTileSide) * kNibbleBits);
}

/// Makes the shapes once.
void make_shapes()
{
   if (g_made)
   {
      return;
   }
   g_made = true;
   const int half = kBallSide / kHalfDivisor;
   for (int y = 0; y < kBallSide; ++y)
   {
      for (int x = 0; x < kBallSide; ++x)
      {
         const int dx = x - half;
         const int dy = y - half;
         const int r2 = dx * dx + dy * dy;
         if (r2 >= kBallRadius2)
         {
            continue;
         }
         const bool marker = y < kMarkerRows + 1 && dx >= -kMarkerHalfWidth && dx <= kMarkerHalfWidth;
         put_pixel(g_ball, kBallSide, x, y, marker ? kMarker : (r2 > kRingRadius2 ? kRing : kBody));
      }
   }
   for (int y = 0; y < kBoxSide; ++y)
   {
      for (int x = 0; x < kBoxSide; ++x)
      {
         const bool edge = x == 0 || y == 0 || x == kBoxSide - 1 || y == kBoxSide - 1;
         put_pixel(g_box, kBoxSide, x, y, edge ? kEdge : (x < kBoxSide / kHalfDivisor ? kBody : kRing));
      }
   }
   for (int r = 0; r < kTileSide; ++r)
   {
      for (int c = 0; c < kTileSide; ++c)
      {
         g_bg_tiles[kStripeTile * kTileWords + r] |= static_cast<uint32_t>(c % kStripes + 1) << (c * kNibbleBits);
         const bool a = (r < kCheckHalf) == (c < kCheckHalf);
         g_bg_tiles[kCheckTile * kTileWords + r] |= (a ? kCheckA : kCheckB) << (c * kNibbleBits);
      }
   }
   for (int i = 0; i < kSineEntries; ++i)
   {
      g_sine[i] = static_cast<int16_t>(capsule::sin_q14(static_cast<uint16_t>(i << kAngleShift)) >> kSineToQ8);
   }
}

/// A 16-colour bank: transparent, then the shapes' colours from a base hue.
/// @param r  red of the body
/// @param g  green
/// @param b  blue
/// @param[out] out  16 colours
void bank_colors(uint8_t r, uint8_t g, uint8_t b, video::Color* out)
{
   constexpr uint8_t kRingShade = 2;
   for (int i = 0; i < video::kColorsPerBank; ++i)
   {
      out[i] = 0;
   }
   out[kBody] = video::rgb5(r, g, b);
   out[kRing] = video::rgb5(static_cast<uint8_t>(r / kRingShade), static_cast<uint8_t>(g / kRingShade),
                            static_cast<uint8_t>(b / kRingShade));
   out[kMarker] = kWhite;
   out[kEdge] = kYellow;
}

/// Sets up the background layers: BG0 stripes (front), BG1 checks.
void backgrounds()
{
   video::Color pal[video::kColorsPerBank] = {kBackdrop};
   for (int i = 1; i <= kStripes; ++i)
   {
      pal[i] = video::rgb5(static_cast<uint8_t>(i * kStripeRed), static_cast<uint8_t>(kStripeGreen - i * kStripeGreenStep),
                           static_cast<uint8_t>(kStripeBlue + i));
   }
   pal[kCheckA] = kCheckColorA;
   pal[kCheckB] = kCheckColorB;
   palette::load_bg(0, pal, video::kColorsPerBank);
   layers::init(layers::LayerTemplate{0, 0, kStripeScreenBlock, 0, 0, kStripePriority, 0});
   layers::init(layers::LayerTemplate{1, 0, kCheckScreenBlock, 0, 0, kCheckPriority, 0});
   video::load_bg_tiles(0, 0, g_bg_tiles, kBgTileCount);
   for (int y = 0; y < kMapCells; ++y)
   {
      for (int x = 0; x < kMapCells; ++x)
      {
         video::write_map_cell(0, x, y, video::map_cell(kStripeTile, 0));
         video::write_map_cell(1, x, y, video::map_cell(kCheckTile, 0));
      }
   }
   layers::show(0, true);
   layers::show(1, true);
}

/// Everything back to the start of a scene.
void clean()
{
   tasks::reset();
   sp::reset();
   af::reset();
   sl::clear();
   layers::reset();
   palette::reset();
   video::set_blend(video::BlendMode::Off, 0, 0);
   video::set_blend_alpha(0, 0);
   video::set_windows(false, false, false);
   for (int bg = 0; bg < video::kNumBgs; ++bg)
   {
      video::configure_bg(static_cast<uint8_t>(bg), false, 0, 0, 0, false, 0);
      video::set_bg_scroll(static_cast<uint8_t>(bg), 0, 0);
   }
   video::set_mode(0);
   palette::load_bg(0, &kBackdrop, 1);
}

// --- scene 0: affine ----------------------------------------------------------------------------------------

/// A ball sprite (2x box).
const sp::Template kBallBig = {kBallTag, kBallTag, video::OBJ_SQUARE, 2, kSpritePriority, 0, sp::kAffineDouble,
                               video::ObjMode::Normal, 0, nullptr, nullptr, nullptr};
/// A ball sprite (its own box).
const sp::Template kBallNormal = {kBallTag, kBallTag, video::OBJ_SQUARE, 2, kSpritePriority, 0,
                                  sp::kAffineNormal, video::ObjMode::Normal, 0, nullptr, nullptr, nullptr};
/// The box sprite.
const sp::Template kBox = {kBoxTag, kBallTag, video::OBJ_SQUARE, 1, kSpritePriority, 0, sp::kAffineNormal,
                           video::ObjMode::Normal, 0, nullptr, nullptr, nullptr};

const af::Cmd kSpin[] = {af::frame(0, 0, 4, 1), af::jump(0)};   ///< Turns 4 steps a frame.
/// Grows and shrinks three times, then holds.
const af::Cmd kPulse[] = {af::frame(0x100, 0x100, 0, 0), af::loop(0), af::frame(16, 16, 0, 8),
                          af::frame(-16, -16, 0, 8),     af::loop(3), af::end()};
/// Squashes and stretches forever.
const af::Cmd kSquash[] = {af::frame(0x100, 0x100, 0, 0), af::frame(-8, 8, 0, 12), af::frame(8, -8, 0, 12),
                           af::jump(1)};
const af::Cmd* const kSpinTable[] = {kSpin};       ///< Spin.
const af::Cmd* const kPulseTable[] = {kPulse};     ///< Pulse.
const af::Cmd* const kSquashTable[] = {kSquash};   ///< Squash.
uint8_t g_box_matrix = af::kNoMatrix;              ///< The box's matrix.

/// Scene 0's setup.
void affine_setup()
{
   backgrounds();
   layers::show(0, false);
   video::Color c[video::kColorsPerBank];
   bank_colors(kStrong, kMid, kWeak, c);
   sp::load_sheet_words(kBallTag, g_ball, kBallTiles);
   sp::load_sheet_words(kBoxTag, g_box, kBoxTiles);
   sp::load_palette(kBallTag, c);
   anim::create_sprite(anim::SpriteTemplate{&kBallBig, kSpinTable}, kQuarterX, kTopY, 0);
   anim::create_sprite(anim::SpriteTemplate{&kBallBig, kPulseTable}, kThreeQuarterX, kTopY, 0);
   anim::create_sprite(anim::SpriteTemplate{&kBallNormal, kSquashTable}, kQuarterX, kBottomY, 0);
   const uint8_t box = anim::create_sprite(anim::SpriteTemplate{&kBox, nullptr}, kThreeQuarterX, kBottomY, 0);
   g_box_matrix = sp::sprite(box).matrix;
}

/// Scene 0's frame.
/// @param t  the scene's frame
void affine_frame(int t)
{
   sp::animate();
   // the box: its matrix set by hand each frame (shrinking x, turning)
   af::set_rotation_scaling(g_box_matrix, static_cast<int16_t>(af::kScaleOne + t), af::kScaleOne,
                            static_cast<uint16_t>(t << kTurnStepShift));
   sp::build_oam();
}

// --- scene 1: scanline --------------------------------------------------------------------------------------

/// Scene 1's setup.
void scanline_setup()
{
   backgrounds();
}

/// Scene 1's frame.
/// @param t  the scene's frame
void scanline_frame(int t)
{
   if (t == 0)
   {
      sl::start(video::LineRegister::Bg0Hofs, video::LineWidth::Half);
      sl::start_wave(sl::Wave{g_sine, 0, video::kScreenHeight, kWaveFrequency, kWaveAmplitude, kWaveDelay});
   }
   else if (t == kPartFrames)
   {
      sl::stop();
      sl::start(video::LineRegister::BldAlpha, video::LineWidth::Half);
      video::set_blend(video::BlendMode::Alpha, kBg0Bit, kBg1Bit);
   }
   else if (t == kHalfDivisor * kPartFrames)
   {
      sl::stop();
      video::set_blend(video::BlendMode::Off, 0, 0);
      sl::start(video::LineRegister::Win0H, video::LineWidth::Half);
      video::set_window_rect(0, 0, 0, 0, video::kScreenHeight);
      video::set_window_masks(kBg0Bit, 0, kBg1Bit, 0);
      video::set_windows(true, false, false);
   }
   tasks::run();
   uint16_t* back = sl::back16();
   if (t >= kPartFrames && t < kHalfDivisor * kPartFrames)
   {
      for (int y = 0; y < sl::kEntries; ++y)
      {
         const int a = (y + t) % kWeightCycle;
         back[y] = static_cast<uint16_t>(a | ((video::kBlendMax - a) << kEvbShift));
      }
   }
   else if (t >= kHalfDivisor * kPartFrames)
   {
      const int r = kDiamondStart + (t - kHalfDivisor * kPartFrames);
      for (int y = 0; y < sl::kEntries; ++y)
      {
         const int dy = y < kCentreY ? kCentreY - y : y - kCentreY;
         const int hw = r > dy ? r - dy : 0;
         const int left = kCentreX - hw;
         const int right = kCentreX + hw;
         back[y] = hw == 0 ? uint16_t{0} : static_cast<uint16_t>((left << video::kWindowLineShift) | right);
      }
   }
}

// --- scene 2: blend -----------------------------------------------------------------------------------------

/// Scene 2's setup.
void blend_setup()
{
   backgrounds();
   sp::load_sheet_words(kBallTag, g_ball, kBallTiles);
   video::Color c[video::kColorsPerBank];
   bank_colors(kStrong, kWeak, kWeak, c);
   sp::load_palette(kRedTag, c);
   bank_colors(kWeak, kStrong, kWeak, c);
   sp::load_palette(kGreenTag, c);
   bank_colors(kWeak, kWeak, kStrong, c);
   sp::load_palette(kBlueTag, c);
   static const sp::Template kRed = {kBallTag, kRedTag, video::OBJ_SQUARE, 2, 1, 0, sp::kAffineOff,
                                     video::ObjMode::Normal, 0, nullptr, nullptr, nullptr};
   static const sp::Template kGreen = {kBallTag, kGreenTag, video::OBJ_SQUARE, 2, 1, 0, sp::kAffineOff,
                                       video::ObjMode::Normal, 0, nullptr, nullptr, nullptr};
   static const sp::Template kBlue = {kBallTag, kBlueTag, video::OBJ_SQUARE, 2, 1, 0, sp::kAffineOff,
                                      video::ObjMode::Normal, 0, nullptr, nullptr, nullptr};
   sp::create(kRed, kQuarterX, kCentreY, 0);
   sp::create(kGreen, kCentreX, kCentreY, 0);
   sp::create(kBlue, kThreeQuarterX, kCentreY, 0);
}

blend::Ramp g_ramp{};   ///< The green ball's ramp.

/// Scene 2's frame.
/// @param t  the scene's frame
void blend_frame(int t)
{
   if (t < kBlendPartFrames)
   {
      // red: a triangle wave toward white; green: a ramp to black and back; blue: its bank only, every 8 frames
      const int phase = t % kTriangle;
      const int tri = phase < kTriangle / kHalfDivisor ? phase : kTriangle - phase;
      blend::tag(kRedTag, static_cast<uint8_t>(tri), kWhite);
      if (t == 0)
      {
         g_ramp = blend::Ramp{blend::tag_mask(kGreenTag), kBlack, 0, video::kBlendMax, 1, 0};
      }
      if (!blend::step(g_ramp))
      {
         // done: back the other way
         const uint8_t to = g_ramp.target == 0 ? uint8_t{video::kBlendMax} : uint8_t{0};
         g_ramp = blend::Ramp{g_ramp.mask, kBlack, g_ramp.target, to, 1, 0};
      }
      if (t % kTileSide == 0)
      {
         blend::banks_unfaded(blend::tag_mask(kBlueTag) | palette::bg_bank(0),
                              static_cast<uint8_t>(t % video::kBlendMax), kBlack);
      }
   }
   else if (t == kBlendPartFrames)
   {
      const uint16_t tags[] = {kRedTag, kGreenTag, kBlueTag};
      blend::fade_tags(tags, kFadeTags, palette::bg_bank(0), 1, 0, video::kBlendMax, kBlack);
   }
   else
   {
      fade::update();
   }
   sp::animate();
   sp::build_oam();
}

// --- scene 3: script ----------------------------------------------------------------------------------------

anim::Player g_player;            ///< The script's player.
anim::Bank g_bank;                ///< The shapes bank.
uint8_t g_script_sprites[kScriptSpriteSlots];   ///< Sprites the script made, in order.
int g_script_made = 0;            ///< How many.
int g_restart_wait = 0;           ///< Frames before the next run.

/// The ball's callback: its first call copies its lifetime (argument 2), then counts it down.
/// @param s  the sprite
void ball_cb(sp::Sprite& s)
{
   constexpr int kLife = 0;
   constexpr int kStarted = 1;
   constexpr int kLifeArg = 2;
   if (s.data[kStarted] == 0)
   {
      s.data[kStarted] = 1;
      s.data[kLife] = g_player.args[kLifeArg];
      return;
   }
   if (--s.data[kLife] <= 0)
   {
      anim::finish_sprite(g_player, static_cast<uint8_t>(&s - &sp::sprite(0)));
   }
}

/// The script's ball (2x box, turning).
const sp::Template kScriptBall = {kBallTag, kBallTag, video::OBJ_SQUARE, 2, 1, 0, sp::kAffineDouble,
                                  video::ObjMode::Normal, 0, nullptr, nullptr, ball_cb};
/// The script's see-through ball.
const sp::Template kGhost = {kBallTag, kBallTag, video::OBJ_SQUARE, 2, 0, 0, sp::kAffineOff,
                             video::ObjMode::SemiTransparent, 0, nullptr, nullptr, ball_cb};
/// The templates (the bank's indices: ball 0, ghost 1).
const anim::SpriteTemplate kScriptTemplates[] = {{&kScriptBall, kSpinTable}, {&kGhost, nullptr}};

/// Orbit task: moves the script's sprite (argument 0) round the centre at a radius (1) for some frames (2).
/// @param id  the task
void orbit(uint8_t id)
{
   constexpr int kSprite = 0;
   constexpr int kRadius = 1;
   constexpr int kFrames = 2;
   constexpr int kAngle = 3;
   constexpr int kStarted = 4;
   int16_t* d = tasks::task(id).data;
   if (d[kStarted] == 0)
   {
      d[kStarted] = 1;
      d[kSprite] = g_player.args[kSprite];
      d[kRadius] = g_player.args[kRadius];
      d[kFrames] = g_player.args[kFrames];
      return;
   }
   d[kAngle] = static_cast<int16_t>(d[kAngle] + kOrbitStep);
   const uint16_t angle = static_cast<uint16_t>(d[kAngle] << kAngleShift);
   if (d[kSprite] < g_script_made)
   {
      sp::Sprite& s = sp::sprite(g_script_sprites[d[kSprite]]);
      if (s.in_use)
      {
         s.x2 = static_cast<int16_t>((capsule::cos_q14(angle) * d[kRadius]) >> capsule::kQ14Shift);
         s.y2 = static_cast<int16_t>((capsule::sin_q14(angle) * d[kRadius]) >> capsule::kQ14Shift);
      }
   }
   if (--d[kFrames] <= 0)
   {
      anim::finish_task(g_player, id);
   }
}

/// Flash task: a palette tag (argument 0) ramped to white and back, each amount held (1) + 1 frames.
/// @param id  the task
void flash(uint8_t id)
{
   constexpr int kTag = 0;
   constexpr int kCoef = 1;
   constexpr int kDelay = 2;
   constexpr int kWait = 3;
   constexpr int kDown = 4;
   constexpr int kStarted = 5;
   constexpr int kFull = 16;
   int16_t* d = tasks::task(id).data;
   if (d[kStarted] == 0)
   {
      d[kStarted] = 1;
      d[kTag] = g_player.args[0];
      d[kDelay] = g_player.args[1];
      return;
   }
   if (d[kWait] > 0)
   {
      --d[kWait];
      return;
   }
   d[kWait] = d[kDelay];
   blend::tag(static_cast<uint16_t>(d[kTag]), static_cast<uint8_t>(d[kCoef]), kWhite);
   d[kCoef] = static_cast<int16_t>(d[kCoef] + (d[kDown] != 0 ? -1 : 1));
   if (d[kCoef] > kFull)
   {
      d[kCoef] = kFull;
      d[kDown] = 1;
   }
   else if (d[kCoef] < 0)
   {
      anim::finish_task(g_player, id);
   }
}

const tasks::Func kScriptTasks[] = {orbit, flash};   ///< Visual tasks (orbit 0, flash 1).
/// The script's tables.
const anim::Tables kScriptTables = {kScriptTemplates, 2, kScriptTasks, 2, nullptr, 0, nullptr, 0, nullptr, 0, {}};

/// CreateSprite: at arguments 0, 1, subpriority = the flags byte.
/// @param p      the player
/// @param t      the template
/// @param flags  subpriority
/// @return the sprite
uint8_t place(anim::Player& p, const anim::SpriteTemplate& t, uint8_t flags)
{
   const uint8_t id = anim::create_sprite(t, p.args[0], p.args[1], flags);
   if (id != sp::kNoSprite && g_script_made < static_cast<int>(sizeof(g_script_sprites)))
   {
      g_script_sprites[g_script_made++] = id;
   }
   return id;
}

/// LoadGfx: the ball's sheet and palette.
/// @param tag  the tag
void load_gfx(anim::Player&, uint16_t tag)
{
   if (tag != kBallTag || sp::sheet_start(kBallTag) != sp::kNoTiles)
   {
      return;
   }
   sp::load_sheet_words(kBallTag, g_ball, kBallTiles);
   video::Color c[video::kColorsPerBank];
   bank_colors(kGoldR, kGoldG, kGoldB, c);
   sp::load_palette(kBallTag, c);
}

/// The script's hooks.
const anim::Hooks kScriptHooks = {place, nullptr, nullptr, nullptr, load_gfx, nullptr};
/// The script's game: the engine's commands only.
constexpr anim::CommandTable kScriptCommands = anim::generic_table();
/// Its setup.
const anim::Game kScriptGame = {kScriptCommands.at, &kScriptTables, &kScriptHooks};

/// Starts the script.
void start_script()
{
   g_script_made = 0;
   anim::start(g_player, g_bank, kScriptGame, anim::find_script(g_bank, "show"));
}

/// Scene 3's setup.
void script_setup()
{
   backgrounds();
   anim::open(kShapesBank, kShapesBank_size, g_bank);
   start_script();
   g_restart_wait = 0;
}

/// Scene 3's frame.
void script_frame()
{
   if (!anim::running(g_player))
   {
      if (g_restart_wait == 0)
      {
         if (g_player.error == anim::PlayerError::None)
         {
            ++g_report.scripts_ended;
         }
         else
         {
            ++g_report.player_errors;
         }
      }
      if (++g_restart_wait >= kRestartFrames)
      {
         g_restart_wait = 0;
         start_script();
      }
   }
   anim::frame(g_player);
   tasks::run();
   sp::animate();
   sp::build_oam();
}

}   // namespace

void frame(uint32_t f)
{
   const int scene = static_cast<int>(f / kSceneFrames) % kScenes;
   const int t = static_cast<int>(f % kSceneFrames);
   if (t == 0)
   {
      make_shapes();
      clean();
      switch (scene)
      {
      case kSceneAffine:
         affine_setup();
         break;
      case kSceneScanline:
         scanline_setup();
         break;
      case kSceneBlend:
         blend_setup();
         break;
      default:
         script_setup();
         break;
      }
   }
   switch (scene)
   {
   case kSceneAffine:
      affine_frame(t);
      break;
   case kSceneScanline:
      scanline_frame(t);
      break;
   case kSceneBlend:
      blend_frame(t);
      break;
   default:
      script_frame();
      break;
   }
   if (sl::active())
   {
      ++g_report.scanline_frames;
   }
   sl::present();   // the work that lands with the frame
}

const Report& report()
{
   return g_report;
}

}   // namespace sandbox_anim
