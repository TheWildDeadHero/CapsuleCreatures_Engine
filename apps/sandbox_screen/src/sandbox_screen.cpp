/// @file
/// The screen framework's sandbox (UC, D-33; simple shapes): the field is a map (data/world.json) with a walker
/// sprite in the middle; the D-pad scrolls it. A opens the list screen over it (a fade to black, four setup frames,
/// a fade from black): a tiled layer with two palette banks, a framed panel, a title window, six animated icons
/// from a sheet (two frames each) and a cursor (UP / DOWN, a task bobs it). A there opens the detail screen over the
/// list (the list's steps run its fade; then the list leaves): a two-frame picture sprite from images (its frames
/// copied into its own tiles), a diagonally scrolling layer and a window naming the icon. B closes the top screen:
/// the detail screen gives the list back (set up again, its cursor kept), the list gives the field back without
/// entering the map again (its camera, its view, the walker as they were; the field's own fade from black).
/// UM: the list's panel map and the detail's pattern map are screen arena blocks (capsule/screen/arena.h): the list
/// keeps its block while the detail screen shows above it; each goes back when its screen closes (the field leaves
/// the arena empty).
/// Laid out like a game: the engine library, reference/project.json naming data/, this scene code.
#include "capsule/asset/asset.h"
#include "capsule/core/memory.h"
#include "capsule/gfx/palette.h"
#include "capsule/gfx/sprites.h"
#include "capsule/hal/input.h"
#include "capsule/hal/system.h"
#include "capsule/hal/video.h"
#include "capsule/map/map.h"
#include "capsule/screen/arena.h"
#include "capsule/screen/fade.h"
#include "capsule/screen/layers.h"
#include "capsule/screen/screen.h"
#include "capsule/screen/sprites.h"
#include "capsule/screen/tasks.h"
#include "capsule/text/encode.h"
#include "capsule/text/window.h"
#include "capsule/ui/keys.h"

#include <cstdint>

namespace
{

namespace asset = capsule::asset;
namespace input = capsule::hal::input;
namespace layers = capsule::screen::layers;
namespace map = capsule::map;
namespace palette = capsule::gfx::palette;
namespace screen = capsule::screen;
namespace sp = capsule::screen::sprites;
namespace tasks = capsule::screen::tasks;
namespace text = capsule::text;
namespace ui = capsule::ui;
namespace video = capsule::hal::video;
namespace window = capsule::text::window;

/// @name The field
///@{
constexpr uint8_t kWalkerSlot = 0;     ///< The walker's gfx::sprites slot.
constexpr int16_t kWalkerX = 112;      ///< Its left edge (the screen's middle).
constexpr int16_t kWalkerY = 72;       ///< Its top edge.
constexpr int32_t kMapPixels = 256;    ///< The map's side (16 metatiles of 16 pixels).
constexpr int32_t kMaxCameraX = kMapPixels - video::kScreenWidth;    ///< The camera's right limit.
constexpr int32_t kMaxCameraY = kMapPixels - video::kScreenHeight;   ///< Its lower limit.
///@}

/// @name Video plan of the two screens
///@{
constexpr video::BgIndex kTextBg = 0;      ///< The windows.
constexpr video::BgIndex kPanelBg = 1;     ///< The list's panel.
constexpr video::BgIndex kPatternBg = 3;   ///< The tiled layer.
constexpr uint8_t kPatternBlock = 0;       ///< Tiles of the panel and pattern layers.
constexpr uint8_t kTextBlock = 2;          ///< Tiles of the windows.
constexpr uint8_t kTextScreen = 31;        ///< Map of the windows.
constexpr uint8_t kPanelScreen = 30;       ///< Map of the panel.
constexpr uint8_t kPatternScreen = 29;     ///< Map of the pattern.
constexpr uint16_t kBorderTile = 3;        ///< The pattern sheet's panel border tile.
constexpr uint16_t kInsideTile = 4;        ///< Its panel inside tile.
constexpr uint8_t kPanelBank = 1;          ///< The panel's palette bank.
constexpr uint8_t kTextBank = 3;           ///< The windows' palette bank.
constexpr uint16_t kWindowTile = 1;        ///< The windows' first tile in their block.
constexpr int kPanelX = 2;                 ///< The panel's left column.
constexpr int kPanelY = 4;                 ///< Its top row.
constexpr int kPanelW = 26;                ///< Its columns.
constexpr int kPanelH = 14;                ///< Its rows.
constexpr uint8_t kInk = 2;                ///< The windows' ink entry.
constexpr uint8_t kShadow = 3;             ///< Their shadow entry.
constexpr uint8_t kPaper = 1;              ///< Their background entry.
constexpr uint8_t kTitleX = 1;             ///< The title window's left column.
constexpr uint8_t kTitleY = 1;             ///< Its top row.
constexpr uint8_t kTitleW = 20;            ///< Its columns.
constexpr uint8_t kTitleH = 2;             ///< Its rows.
constexpr int kTextLeft = 4;               ///< Pixels from a window's edge to its text.
constexpr int kTextTop = 2;                ///< Pixels from its top.
///@}

/// @name The list's sprites
///@{
constexpr uint16_t kIconTag = 1;       ///< The icon sheet's tag.
constexpr uint16_t kCursorTag = 2;     ///< The cursor sheet's tag.
constexpr uint16_t kObjTag = 1;        ///< The sprites' palette tag.
constexpr int kIcons = 6;              ///< Icons on the panel.
constexpr int kIconColumns = 3;        ///< Icons per row.
constexpr int16_t kIconX0 = 64;        ///< The first icon's centre x.
constexpr int16_t kIconY0 = 64;        ///< Its centre y.
constexpr int16_t kIconDx = 56;        ///< Between columns.
constexpr int16_t kIconDy = 48;        ///< Between rows.
constexpr int16_t kCursorDx = -20;     ///< The cursor left of its icon.
constexpr uint8_t kIconFrameTicks = 12;   ///< Frames of each icon frame.
constexpr int16_t kSecondIconTiles = 8;   ///< The second icon's first tile in the sheet (two 2 x 2 frames on).
constexpr int16_t kSecondFrame = 4;       ///< An icon's second frame (tiles on).
constexpr uint8_t kSubpriority = 2;    ///< The icons' subpriority (the cursor's is lower: in front).
constexpr int16_t kBobFrames = 8;      ///< The cursor moves a pixel every 8 frames.
constexpr int16_t kBobSpan = 2;        ///< ... between 0 and 2.
constexpr uint8_t kTaskPriority = 1;   ///< The bob task's priority.
///@}

/// @name The detail screen
///@{
constexpr uint16_t kPictureTiles = 64;   ///< A 64 x 64 frame.
constexpr uint16_t kPictureFrames = 2;   ///< Frames of the picture.
constexpr uint8_t kPictureTicks = 20;    ///< Frames of each picture frame.
constexpr int16_t kPictureX = 120;       ///< Its centre.
constexpr int16_t kPictureY = 88;        ///< Its centre.
constexpr int16_t kScrollStep = 1;       ///< The pattern moves a pixel a frame.
constexpr int kDigitCode = '0';          ///< The font's digit codes start here.
///@}

constexpr int kMapCells = layers::kRowCells * layers::kRowCells;   ///< Cells of one 32 x 32 map.
constexpr uint8_t kSize64 = 3;          ///< OBJ size 3 (64 x 64 square).
constexpr int kBorders = 2;             ///< A panel's two border cells across (down).
constexpr uint8_t kListTitleState = 2;  ///< The list setup's title step.
constexpr int kIconKinds = 2;           ///< Icon animations, alternating.
constexpr ui::Repeat kRepeat = {40, 5};   ///< The pad's repeat timing.

// ---- shared state ----

/// The program's state.
struct State
{
   ui::KeyState keys;        ///< The pad's state.
   int32_t cam_x;            ///< The field's camera.
   int32_t cam_y;            ///< The field's camera.
   int cursor;               ///< The list's cursor (kept while the detail screen shows).
   uint8_t cursor_sprite;    ///< Its sprite.
   uint8_t picture;          ///< The detail screen's picture sprite.
   int16_t scroll;           ///< The detail screen's pattern offset.
};
State g{};   ///< The sandbox's state.

CAPSULE_EWRAM_BSS uint16_t g_text_map[kMapCells];      ///< BG0's map.
CAPSULE_EWRAM_BSS uint16_t g_pattern_map[kMapCells];   ///< BG3's map on the list.

/// One layer's map buffer (a screen arena block).
struct LayerMap
{
   uint16_t cells[kMapCells];   ///< The map.
};
screen::Block<LayerMap> g_panel_map;     ///< The list's BG1 map (kept while the detail screen shows).
screen::Block<LayerMap> g_detail_map;    ///< The detail's own BG3 map.
CAPSULE_EWRAM_BSS window::Window g_title;   ///< The screens' title window.
CAPSULE_EWRAM_BSS window::TileBuffer<kTitleW, kTitleH> g_title_tiles;   ///< C13c: its tiles.

const text::Code kListTitle[] = {'L', 'I', 'S', 'T', ' ', 'S', 'C', 'R', 'E', 'E', 'N', text::kCodeEnd};   ///< Title.
text::Code g_detail_title[] = {'D', 'E', 'T', 'A', 'I', 'L', ' ', '0', text::kCodeEnd};   ///< Its digit set.
constexpr int kDetailDigit = 7;   ///< The digit's index in g_detail_title.

// ---- animations and templates ----

constexpr sp::AnimCmd kIconA[] = {{sp::AnimOp::Frame, kIconFrameTicks, 0, 0, 0},
                                  {sp::AnimOp::Frame, kIconFrameTicks, 0, 0, kSecondFrame},
                                  {sp::AnimOp::Jump, 0, 0, 0, 0}};   ///< The first icon's frames.
constexpr sp::AnimCmd kIconB[] = {{sp::AnimOp::Frame, kIconFrameTicks, 0, 0, kSecondIconTiles},
                                  {sp::AnimOp::Frame, kIconFrameTicks, sp::kFlipH, 0, kSecondIconTiles + kSecondFrame},
                                  {sp::AnimOp::Jump, 0, 0, 0, 0}};   ///< The second icon's frames.
constexpr const sp::AnimCmd* kIconAnims[] = {kIconA, kIconB};       ///< The icons' animations.
constexpr sp::AnimCmd kPictureAnim[] = {{sp::AnimOp::Frame, kPictureTicks, 0, 0, 0},
                                        {sp::AnimOp::Frame, kPictureTicks, 0, 0, 1},
                                        {sp::AnimOp::Jump, 0, 0, 0, 0}};   ///< The picture's two frames.
constexpr const sp::AnimCmd* kPictureAnims[] = {kPictureAnim};      ///< Its animation.

/// The cursor's bob: a pixel right every kBobFrames frames, back after kBobSpan.
/// @param s  the cursor
void bob(sp::Sprite& s)
{
   if (++s.data[0] < kBobFrames)
   {
      return;
   }
   s.data[0] = 0;
   s.x2 = static_cast<int16_t>(s.x2 >= kBobSpan ? 0 : s.x2 + 1);
}

constexpr sp::Template kIconTemplate = {kIconTag, kObjTag, video::OBJ_SQUARE, 1, 1, 0, sp::kAffineOff,
                                        video::ObjMode::Normal, 0, kIconAnims, nullptr, nullptr};   ///< An icon.
constexpr sp::Template kCursorTemplate = {kCursorTag, kObjTag, video::OBJ_SQUARE, 1, 0, 0, sp::kAffineOff,
                                          video::ObjMode::Normal, 0, nullptr, nullptr, bob};      ///< The cursor.
sp::Images g_picture_images{};   ///< The picture's frames (from its tiles asset).
sp::Template g_picture_template = {sp::kNoTag, kObjTag, video::OBJ_SQUARE, kSize64, 1, 0, sp::kAffineOff,
                                   video::ObjMode::Normal, 0, kPictureAnims, &g_picture_images, nullptr};   ///< It.

/// A tiles asset by key.
/// @param key  its key
/// @return it (the sandbox's data has it)
const asset::Tiles& tiles(const char* key)
{
   return *asset::tiles(asset::key_v1(key));
}

/// A palette asset by key.
/// @param key  its key
/// @return it
const asset::Palette& colors(const char* key)
{
   return *asset::palette(asset::key_v1(key));
}

// ---- the screens' common parts ----

constexpr layers::LayerTemplate kListLayers[] = {
   {kTextBg, kTextBlock, kTextScreen, 0, 0, 0, 0},
   {kPanelBg, kPatternBlock, kPanelScreen, 0, 0, 1, 0},
   {kPatternBg, kPatternBlock, kPatternScreen, 0, 0, video::kNumPriorities - 1, 0}};   ///< The list's layers.
constexpr layers::Plan kListPlan = {kListLayers, 3, 0};   ///< The list's plan.
constexpr layers::LayerTemplate kDetailLayers[] = {
   {kTextBg, kTextBlock, kTextScreen, 0, 0, 0, 0},
   {kPatternBg, kPatternBlock, kPatternScreen, 0, 0, video::kNumPriorities - 1, 0}};   ///< The detail's layers.
constexpr layers::Plan kDetailPlan = {kDetailLayers, 2, 0};   ///< The detail's plan.

/// Loads the shared graphics: the pattern tiles, the BG palette, the pattern layer.
/// @param pattern  the pattern layer's map buffer
void load_common(uint16_t* pattern)
{
   tasks::reset();
   sp::reset();
   layers::set_buffer(kTextBg, g_text_map);
   layers::set_buffer(kPatternBg, pattern);
   layers::load_tiles(kPatternBg, tiles("tiles:SANDBOX_SCREEN_PATTERN"), 0);
   const asset::Palette& bg = colors("palette:SANDBOX_SCREEN_BG");
   palette::load_bg(0, bg.colors, bg.count);
   layers::set_tilemap(kPatternBg, *asset::tilemap(asset::key_v1("tilemap:SANDBOX_SCREEN_PATTERN")));
   layers::copy_to_vram(kPatternBg);
   layers::fill_linear(kTextBg, 0, 0, 0, layers::kRowCells, layers::kRowCells);
}

/// Opens the title window and prints a title in it (its cells mirrored into the text layer's map).
/// @param title  the text
void open_title(const text::Code* title)
{
   window::Config c;
   c.font = asset::font(asset::key_v1("font:SANDBOX_SCREEN"));
   c.bg = kTextBg;
   c.char_block = kTextBlock;
   c.first_tile = kWindowTile;
   c.palette_bank = kTextBank;
   c.x = kTitleX;
   c.y = kTitleY;
   c.width = kTitleW;
   c.height = kTitleH;
   c.ink = kInk;
   c.shadow = kShadow;
   c.background = kPaper;
   c.frames_per_char = 0;
   window::attach(g_title, g_title_tiles.tiles());
   window::open(g_title, c);
   window::print_at(g_title, kTextLeft, kTextTop, title);
   layers::put_window(kTextBg, kTitleX, kTitleY, kTitleW, kTitleH, kWindowTile, kTextBank);
   layers::copy_to_vram(kTextBg);
}

/// Loads the sprites' palette.
void load_obj_palette()
{
   sp::load_palette(kObjTag, colors("palette:SANDBOX_SCREEN_OBJ").colors);
}

/// The screens' Main step: the title window's tiles go up when they change.
void window_main()
{
   window::update(g_title, false, false);
}

// ---- the list screen ----

extern const screen::Screen kDetail;

/// Puts the cursor next to its icon.
void place_cursor()
{
   sp::Sprite& s = sp::sprite(g.cursor_sprite);
   s.x = static_cast<int16_t>(kIconX0 + (g.cursor % kIconColumns) * kIconDx + kCursorDx);
   s.y = static_cast<int16_t>(kIconY0 + (g.cursor / kIconColumns) * kIconDy);
}

/// The bob counter task: counts frames into its data (it shows the task list running).
/// @param id  its entry
void count_task(uint8_t id)
{
   ++tasks::task(id).data[0];
}

/// The list's setup, one part a frame.
/// @param state    its state
/// @param arg      unused
/// @param resumed  set up again after the detail screen
/// @return true when done
bool list_setup(uint8_t& state, uint8_t arg, bool resumed)
{
   (void)arg;
   (void)resumed;   // the cursor is kept in g either way
   switch (state++)
   {
   case 0:
      load_common(g_pattern_map);
      if (!g_panel_map.taken())
      {
         g_panel_map.take();   // the first opening; set up again after the detail screen, the list still has it
      }
      layers::set_buffer(kPanelBg, g_panel_map->cells);
      return false;
   case 1:
      layers::fill(kPanelBg, kBorderTile, kPanelX, kPanelY, kPanelW, kPanelH, kPanelBank);
      layers::fill(kPanelBg, kInsideTile, kPanelX + 1, kPanelY + 1, kPanelW - kBorders, kPanelH - kBorders,
                   kPanelBank);
      layers::copy_to_vram(kPanelBg);
      return false;
   case kListTitleState:
      open_title(kListTitle);
      return false;
   default:
      load_obj_palette();
      sp::load_sheet(kIconTag, tiles("tiles:SANDBOX_SCREEN_ICONS"));
      sp::load_sheet(kCursorTag, tiles("tiles:SANDBOX_SCREEN_CURSOR"));
      for (int i = 0; i < kIcons; ++i)
      {
         const uint8_t id = sp::create(kIconTemplate, static_cast<int16_t>(kIconX0 + (i % kIconColumns) * kIconDx),
                                       static_cast<int16_t>(kIconY0 + (i / kIconColumns) * kIconDy), kSubpriority);
         sp::start_anim(id, static_cast<uint8_t>(i % kIconKinds));
      }
      g.cursor_sprite = sp::create(kCursorTemplate, 0, 0, 0);
      place_cursor();
      tasks::create(count_task, kTaskPriority);
      layers::show(kTextBg, true);
      layers::show(kPanelBg, true);
      layers::show(kPatternBg, true);
      return true;
   }
}

/// The list's input: the cursor, A for the detail screen, B to close.
void list_input()
{
   const ui::Keys& k = screen::keys();
   if (ui::pressed(k, input::KEY_B))
   {
      screen::close();
   }
   else if (ui::pressed(k, input::KEY_A))
   {
      screen::push(kDetail, static_cast<uint8_t>(g.cursor));
   }
   else if (ui::repeated(k, input::KEY_DOWN) || ui::repeated(k, input::KEY_RIGHT))
   {
      g.cursor = (g.cursor + 1) % kIcons;
      place_cursor();
   }
   else if (ui::repeated(k, input::KEY_UP) || ui::repeated(k, input::KEY_LEFT))
   {
      g.cursor = (g.cursor + kIcons - 1) % kIcons;
      place_cursor();
   }
}

/// The list's frame order: the reference games' usual one, then the window.
constexpr screen::Step kListSteps[] = {screen::Step::Tasks, screen::Step::Animate, screen::Step::BuildOam,
                                       screen::Step::CopyTilemaps, screen::Step::Fade, screen::Step::Main};

const screen::Screen kList = {&kListPlan, list_setup, list_input, window_main, nullptr, nullptr, kListSteps,
                              sizeof(kListSteps) / sizeof(kListSteps[0]), screen::kFadeToBlack,
                              screen::kFadeFromBlack, screen::kFadeToBlack};   ///< The list screen.

// ---- the detail screen ----

/// The detail's setup, one part a frame.
/// @param state    its state
/// @param arg      the icon chosen
/// @param resumed  unused (nothing opens over it)
/// @return true when done
bool detail_setup(uint8_t& state, uint8_t arg, bool resumed)
{
   (void)resumed;
   switch (state++)
   {
   case 0:
      load_common(g_detail_map.take().cells);   // above the list's block; back to the arena when it closes
      g.scroll = 0;
      return false;
   case 1:
      g_detail_title[kDetailDigit] = static_cast<text::Code>(kDigitCode + arg + 1);
      open_title(g_detail_title);
      return false;
   default:
   {
      load_obj_palette();
      const asset::Tiles& t = tiles("tiles:SANDBOX_SCREEN_PICTURE");
      g_picture_images = sp::Images{t.words, kPictureTiles, kPictureFrames};
      g.picture = sp::create(g_picture_template, kPictureX, kPictureY, 0);
      layers::show(kTextBg, true);
      layers::show(kPatternBg, true);
      return true;
   }
   }
}

/// The detail's input: B closes.
void detail_input()
{
   if (ui::pressed(screen::keys(), input::KEY_B))
   {
      screen::close();
   }
}

/// The detail's Main step: the pattern scrolls, the window's tiles go up.
void detail_main()
{
   g.scroll = static_cast<int16_t>(g.scroll + kScrollStep);
   layers::set_scroll(kPatternBg, g.scroll, g.scroll);
   window_main();
}

const screen::Screen kDetail = {&kDetailPlan, detail_setup, detail_input, detail_main, nullptr, nullptr, kListSteps,
                                sizeof(kListSteps) / sizeof(kListSteps[0]), screen::kFadeToBlack,
                                screen::kFadeFromBlack, screen::kFadeToBlack};   ///< The detail screen.

// ---- the field ----

/// The field's callback on its return: its own fade from black (the screen's out fade left it black).
/// @return true (one frame)
bool field_callback()
{
   screen::fade::begin(screen::fade::kFromBlack);
   return true;
}

/// The sandbox's host: the engine's default steps, with the field's fade-in.
const screen::Host kHost = {screen::default_host().suspend, screen::default_host().resume, nullptr, nullptr,
                            field_callback};

/// Shows the field: the map, the walker.
void start_field()
{
   palette::reset();
   capsule::gfx::sprites::reset();
   const asset::Sprite* walker = asset::sprite(asset::key_v1("sprite:SANDBOX_SCREEN_WALKER"));
   if (walker != nullptr)
   {
      capsule::gfx::sprites::load(kWalkerSlot, *walker);
      capsule::gfx::sprites::show(kWalkerSlot, true);
      capsule::gfx::sprites::set_position(kWalkerSlot, kWalkerX, kWalkerY);
   }
   map::enter(map::kWorld.maps[0], g.cam_x, g.cam_y);
   screen::set_host(kHost);
}

/// One field frame: the camera, A opens the list.
/// @param k  the frame's keys
void field_frame(const ui::Keys& k)
{
   if (ui::held(k, input::KEY_LEFT) && g.cam_x > 0)
   {
      --g.cam_x;
   }
   if (ui::held(k, input::KEY_RIGHT) && g.cam_x < kMaxCameraX)
   {
      ++g.cam_x;
   }
   if (ui::held(k, input::KEY_UP) && g.cam_y > 0)
   {
      --g.cam_y;
   }
   if (ui::held(k, input::KEY_DOWN) && g.cam_y < kMaxCameraY)
   {
      ++g.cam_y;
   }
   map::set_camera(g.cam_x, g.cam_y);
   if (ui::pressed(k, input::KEY_A) && !screen::fade::active())
   {
      screen::push(kList);
   }
   screen::fade::update();
   capsule::gfx::sprites::update();
   map::update();
}

}   // namespace

/// Runs the sandbox until the platform quits.
/// @return 0
int main()
{
   capsule::hal::system::init();
   start_field();
   while (capsule::hal::system::update())
   {
      const ui::Keys k = ui::read(g.keys, kRepeat, input::held());
      if (screen::active())
      {
         screen::frame(k);   // a screen, or the field coming back
      }
      else
      {
         field_frame(k);
      }
   }
   return 0;
}
