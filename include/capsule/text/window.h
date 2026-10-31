/// @file
/// A text window drawn pixel by pixel with a proportional font (I4, D-29): typed text in a rectangle of a regular
/// background layer, as handheld games draw their message boxes.
///
/// open() gives the window a rectangle of the layer's map and as many tiles of its character block (one per cell,
/// row by row from Config::first_tile) and clears them to the background color. print() clears the window and types
/// a text: one glyph every Config::frames_per_char frames (0: at once; Accept or Back held types the rest of the line
/// at once), each glyph's ink, shadow and fill pixels in the window's colors, advancing by its width and the font's
/// letter spacing. The window's tiles live in RAM; update() copies the rows that changed to VRAM once a frame.
///
/// Codes (capsule/text/encode.h):
/// - newline: the next line; past the last line the window scrolls up a line first;
/// - prompt (FA): waits for a button (the arrow glyph blinks at the end of the line; N3: or the arrow image steps
///   at the pen, Config::arrow_kind), then goes on;
/// - page break (FB): waits for a button, then clears the window and goes on at the first line;
/// - Control::Scroll: waits for a button, then scrolls the window up a line (Config::scroll_speed pixels a frame)
///   and goes on at the start of the last line;
/// - Control::Pause n: waits n frames; Control::WaitButton: waits for a button without the arrow;
/// - Control::Font n: C10: glyphs from Config::fonts[n] (line heights stay the window font's); ResetFont: back;
/// - Control::Color / Highlight / Shadow / Colors: the ink, background and shadow palette entries;
/// - Control::ShiftRight n: the pen to pixel n of the line; Control::ShiftDown n: C13a: the line to pixel n below
///   the text's start (print(): Config::top; print_at(): its y), as the handheld games' printer sets it;
/// - C13a, as the handheld games' printer: Control::Fill: the window filled with the background
///   entry, the pen back to the text's start; Control::Clear n: n pixels at the pen cleared (a glyph's height, the
///   background entry; nothing when it is 0) and the pen moved past them; Control::SkipTo n: the pen to pixel n from
///   the text's start; Control::ClearTo n: Clear up to that pixel (nothing when the pen is there or past it);
///   Control::MinSpacing n: from then on every glyph advances at least n pixels (the rest cleared as Clear does).
///   Clear and ClearTo of a positive width type as a glyph does (they end a typed frame);
/// - E1 (M4), as the handheld games' printer: Control::Se n / Bgm n play the sound or the music, PauseMusic /
///   ResumeMusic pause and resume it (sound::control(): the game's, the default does nothing); Control::WaitSe
///   ends the frame and waits until sound::waiting() is false (the frame that sees it so still waits);
/// - field placeholders (FD 80-FE): the field resolver's text (capsule/text/placeholder.h);
/// - every other control is skipped with its arguments (escape_length()).
///
/// Glyphs (C13a, as the handheld games copy a glyph): a glyph draws its advance width's columns (widths[],
/// at most its cell) and PropFont::glyph_height rows (its cell's when 0), both clipped to the window's edges; its 1
/// pixels take the ink entry, 2 the shadow, 0 and 3 the background; an entry 0 is never written (transparent: a
/// background 0 leaves what is under the glyph).
///
/// C10: how text types is data (Config::typing):
/// - kTypingInterval: one glyph every frames_per_char frames; controls and newlines take no time; Accept or Back held
///   (update()'s held, as the caller decides) types the rest up to the next pause at once.
/// - kTypingCounter: a frame-counted printer as handheld games run one, once a frame: each code read (a glyph, a
///   newline, a control) restarts a counter of char_delay frames, which counts down one a frame before the next
///   code is read (so a glyph shows every char_delay + 1 frames; a code that draws nothing reads on at once but
///   costs the counter too, its first frame included). Control::Pause n waits n frames and then one more before the
///   next code; a scroll ends one frame after its last pixel row; a prompt, page break, scroll or WaitButton waits
///   for a press, then the counter from the code that waited runs out first. With speedup kSpeedupPerFrame an
///   Accept / Back press while the counter runs ends the count, and from then on Accept / Back held keeps it at 0
///   (a glyph a frame); kSpeedupRest keeps kTypingInterval's held rule; kSpeedupNone ignores the keys.
///   lead_frames frames pass after print() before the first code is read (a box that draws its frame first).
///
/// Tile storage (C13c): a Window holds no pixels of its own. Its owner gives it storage for width x height tiles
/// (attach()), as the handheld games allocate exactly that buffer for a window: a static TileBuffer<W, H> of the
/// window's size, a slice of a pool the owner keeps, a piece of a TilePool (any order of taking and giving back), or
/// memory of a screen arena block (capsule/screen/arena.h), given back when the screen leaves as the games free their
/// window buffers (FreeAllWindowBuffers). update(), print() and clear() queue their copies to land at the next
/// VBlank (capsule/hal/vram_queue.h): the storage must stay valid and unchanged until then.
#pragma once

#include "capsule/hal/video.h"
#include "capsule/text/encode.h"
#include "capsule/text/font.h"
#include "capsule/text/placeholder.h"

#include <cstdint>

/// Proportional text windows.
namespace capsule::text::window
{

inline constexpr int kMaxTiles = 256;   ///< Most cells one window covers (N3: 256, was 128; C13c: its owner's tiles).
inline constexpr int kWordsPerTile = 8;   ///< 32-bit words of one 4bpp tile (one per pixel row).
/// Bytes of one 4bpp tile (the handheld games' 32 a tile).
inline constexpr int kBytesPerTile = kWordsPerTile * static_cast<int>(sizeof(uint32_t));
inline constexpr Code kNoArrow = kCodeEnd;   ///< Config::arrow: no arrow glyph.
inline constexpr uint8_t kDefaultInk = 1;      ///< Config::ink by default (a font's ink pixel value).
inline constexpr uint8_t kDefaultShadow = 2;   ///< Config::shadow by default (a font's shadow pixel value).

/// @name Config::typing (C10)
///@{
inline constexpr uint8_t kTypingInterval = 0;   ///< A glyph every frames_per_char frames.
inline constexpr uint8_t kTypingCounter = 1;    ///< A counter of char_delay frames after every code read.
///@}

/// @name Config::arrow_kind (N3: how a prompt, a page break or a scroll shows that it waits)
///@{
inline constexpr uint8_t kArrowGlyph = 0;   ///< Config::arrow blinks at the right end of the window's line.
inline constexpr uint8_t kArrowImage = 1;   ///< An image at the pen (Config::arrow_rows), stepping its rows.
inline constexpr int kArrowImageWidth = 8;  ///< Pixels across an arrow image (one 4bpp word per pixel row).
///@}

/// @name Config::speedup (C10: what Accept / Back does while text types)
///@{
inline constexpr uint8_t kSpeedupRest = 0;       ///< Held: the rest up to the next pause at once.
inline constexpr uint8_t kSpeedupPerFrame = 1;   ///< kTypingCounter: a press ends the count; then held: a glyph a frame.
inline constexpr uint8_t kSpeedupNone = 2;       ///< The keys do not speed it up.
///@}

/// Where and how a window draws.
struct Config
{
   const PropFont* font = nullptr;          ///< The font.
   hal::video::BgIndex bg = 0;              ///< The layer.
   uint8_t char_block = 0;                  ///< Its character block.
   hal::video::TileIndex first_tile = 0;    ///< First tile the window takes (width x height tiles).
   uint8_t palette_bank = 0;                ///< Palette bank of its cells.
   uint8_t x = 0;                           ///< Left cell on the layer's map.
   uint8_t y = 0;                           ///< Top cell.
   uint8_t width = 0;                       ///< Cells across (width x height at most kMaxTiles).
   uint8_t height = 0;                      ///< Cells down.
   uint8_t left = 0;                        ///< Pixels from the window's left edge to a line's start.
   uint8_t top = 0;                         ///< Pixels from its top edge to the first line's top.
   uint8_t lines = 1;                       ///< Lines the window shows.
   uint8_t ink = kDefaultInk;               ///< Palette entry of ink pixels.
   uint8_t shadow = kDefaultShadow;         ///< Palette entry of shadow pixels.
   uint8_t background = 0;                  ///< Palette entry of the window's empty pixels (and fill pixels).
   uint8_t frames_per_char = 1;             ///< Typing speed (0: at once).
   uint8_t scroll_speed = 1;                ///< Pixels a frame a scroll moves (0: at once).
   Code arrow = kNoArrow;                   ///< Glyph shown while a prompt waits, or kNoArrow.
   uint8_t typing = kTypingInterval;        ///< C10: how text types (kTypingInterval, kTypingCounter).
   uint8_t char_delay = 0;                  ///< C10: kTypingCounter: frames counted after each code read.
   uint8_t speedup = kSpeedupRest;          ///< C10: what Accept / Back does (kSpeedupRest, kSpeedupPerFrame, ...).
   uint8_t lead_frames = 0;                 ///< C10: kTypingCounter: frames after print() before the first code.
   uint8_t font_count = 0;                  ///< C10: entries of fonts.
   const PropFont* const* fonts = nullptr;  ///< C10: the fonts Control::Font n selects (n < font_count), or nullptr.
   uint8_t arrow_kind = kArrowGlyph;        ///< N3: how a wait shows its arrow (kArrowGlyph, kArrowImage).
   const uint32_t* arrow_rows = nullptr;    ///< N3: kArrowImage: the image, one 4bpp word (8 pixels) per row.
   uint8_t arrow_image_rows = 0;            ///< N3: rows of arrow_rows.
   uint8_t arrow_height = 0;                ///< N3: rows shown at once (a window onto the image).
   const uint8_t* arrow_offsets = nullptr;  ///< N3: the image row shown first, one entry per step.
   uint8_t arrow_steps = 0;                 ///< N3: entries of arrow_offsets.
   uint8_t arrow_delay = 0;                 ///< N3: frames between two steps (the first step shows at once).
};

/// @name Frame tile parts (Frame::first_tile + row * kFrameColumns + column)
///@{
inline constexpr int kFrameColumns = 3;   ///< Parts per row: the left edge, the inside, the right edge.
inline constexpr int kFrameParts = 9;     ///< Top row, middle row, bottom row of three parts each.
///@}

/// A text box's frame around its window (I4): a one-cell border ring around the window's rectangle grown by a
/// padding of fill cells on each side. A game's box styles are data: the window's Config places the text, the
/// Frame places the border relative to it (for example a message box whose inside starts one cell left of its text).
/// The nine tiles are the game's art, in reading order: top-left, top, top-right, left, inside, right, bottom-left,
/// bottom, bottom-right; the padding's cells take the inside tile, the border's top and bottom rows above and below
/// the padding take the top and bottom tiles.
struct Frame
{
   hal::video::TileIndex first_tile = 0;   ///< The nine parts' first tile (in the window's character block).
   uint8_t palette_bank = 0;               ///< Palette bank of the frame's cells.
   uint8_t pad_left = 0;                   ///< Inside cells between the window's left edge and the border.
   uint8_t pad_top = 0;                    ///< Inside cells above the window.
   uint8_t pad_right = 0;                  ///< Inside cells right of the window.
   uint8_t pad_bottom = 0;                 ///< Inside cells below the window.
};

/// What a window's text waits for.
enum class Pause : uint8_t
{
   None,        ///< Typing (or done).
   Prompt,      ///< A prompt: a button, then go on.
   Page,        ///< A page break: a button, then clear.
   Scroll,      ///< A scroll: a button, then scroll a line.
   Scrolling,   ///< Scrolling a line up.
   Button,      ///< WaitButton: a button, no arrow.
   Frames,      ///< Control::Pause: frames.
   Sound,       ///< E1 (M4): Control::WaitSe: until the sound effect ends (sound::waiting()).
   NUM_PAUSES   ///< Number of pauses.
};

/// C13c: tile storage a window draws into, its owner's memory (see the file comment): kWordsPerTile words a tile,
/// cells row-major, 4-byte aligned. Empty: no storage (a window opened without any stays closed).
struct Tiles
{
   uint32_t* words = nullptr;   ///< The first tile's first word.
   uint16_t count = 0;          ///< Tiles it holds.
};

/// C13c: words of storage for a window of width x height cells.
/// @param width   cells across
/// @param height  cells down
/// @return words (kWordsPerTile a cell)
constexpr int tile_words(int width, int height)
{
   return width * height * kWordsPerTile;
}

/// C13c: storage for a window of kWidth x kHeight cells, sized at compile time (a static, or a member of a screen
/// arena block's struct).
/// @tparam kWidth   cells across
/// @tparam kHeight  cells down
template <int kWidth, int kHeight>
struct TileBuffer
{
   static_assert(kWidth > 0 && kHeight > 0 && kWidth * kHeight <= kMaxTiles,
                 "a window covers 1 to kMaxTiles cells");
   static constexpr int kTiles = kWidth * kHeight;   ///< Tiles it holds.

   uint32_t words[tile_words(kWidth, kHeight)];      ///< The tiles.

   /// The storage, for attach().
   /// @return it
   Tiles tiles()
   {
      return Tiles{words, static_cast<uint16_t>(kTiles)};
   }
};

/// C13c: most pieces a TilePool hands out at once (the handheld games' window limit).
inline constexpr int kMaxPoolPieces = 32;

/// C13c: tiles handed out from one buffer of its owner's (a static, or a screen arena block) in any order, the way
/// the handheld games' window add / remove take window buffers from their heap: pool_take() gives the lowest
/// free stretch that fits (first fit), pool_give_back() returns a piece. Plain data (zero is an empty pool; it fits
/// in a screen arena block); its fields are the functions' business.
struct TilePool
{
   uint32_t* words;                          ///< The buffer.
   uint32_t capacity;                        ///< Its tiles.
   uint32_t high_water;                      ///< The most tiles handed out at once since pool_init().
   uint16_t start[kMaxPoolPieces];           ///< Each piece's first tile.
   uint16_t count[kMaxPoolPieces];           ///< Its tiles (0: the entry is free).
};

/// C13c: makes a pool over a buffer (every piece it handed out before is forgotten).
/// @param p      the pool
/// @param words  the buffer (kWordsPerTile words a tile)
/// @param tiles  its tiles
void pool_init(TilePool& p, uint32_t* words, uint32_t tiles);

/// C13c: hands out tiles: the lowest free stretch that fits (first fit) and that no copy still waiting for the VBlank
/// reads (a piece given back this frame keeps its tiles until its last copy lands, as a window's own buffer would).
/// @param p      the pool
/// @param tiles  how many (at least 1)
/// @return the storage, or an empty Tiles when no stretch fits (or kMaxPoolPieces are out)
Tiles pool_take(TilePool& p, int tiles);

/// C13c: gives a piece back (nothing for an empty Tiles or one the pool did not hand out).
/// @param p  the pool
/// @param t  the piece
void pool_give_back(TilePool& p, const Tiles& t);

/// C13c: tiles handed out now.
/// @param p  the pool
/// @return them
uint32_t pool_used(const TilePool& p);

/// One window (C13c: about 100 bytes; its tiles are its owner's, attach()). Plain data: its fields are the
/// functions' business.
struct Window
{
   Config c;                     ///< Where and how.
   bool open;                    ///< Open.
   const Code* next;             ///< The next code, or nullptr when done.
   const Code* resume;           ///< Where the text goes on after a placeholder's expansion, or nullptr.
   FieldText field;              ///< The placeholder being typed.
   int16_t pen_x;                ///< Pixels from the window's left edge.
   int16_t line_y;               ///< Top of the current line, pixels from the window's top edge.
   uint8_t line;                 ///< Line index, 0 to lines - 1.
   uint8_t ink;                  ///< Current ink entry.
   uint8_t shadow;               ///< Current shadow entry.
   uint8_t background;           ///< Current background entry.
   Pause pause;                  ///< What it waits for.
   uint16_t frames;              ///< Pause::Frames: frames left.
   uint8_t timer;                ///< Frames to the next glyph (kTypingCounter: the counter).
   uint8_t lead;                 ///< C10: kTypingCounter: lead frames left before the first code.
   bool sped_up;                 ///< C10: kSpeedupPerFrame: a press sped this text up.
   const PropFont* glyphs;       ///< C10: the font glyphs come from (Control::Font), c.font after print().
   uint8_t scrolled;             ///< Pause::Scrolling: pixels moved so far.
   uint8_t blink;                ///< Arrow blink counter.
   bool arrow_shown;             ///< The arrow is drawn.
   uint8_t arrow_step;           ///< N3: kArrowImage: the next step of arrow_offsets.
   uint8_t arrow_wait;           ///< N3: kArrowImage: frames left before the next step.
   bool instant;                 ///< print(..., true).
   int16_t start_x;              ///< C13a: the text's start (print(): Config::left; print_at(): its x).
   int16_t start_y;              ///< C13a: its top (print(): Config::top; print_at(): its y).
   uint8_t min_spacing;          ///< C13a: Control::MinSpacing's least advance, 0 when off (each text starts off).
   int dirty_lo;                 ///< First tile changed this frame, or -1.
   int dirty_hi;                 ///< Last tile changed.
   Tiles store;                  ///< C13c: its tiles (its owner's; the RAM copy update() sends to VRAM).
};

/// C13c: gives a window its tile storage (kept until the next attach(); a window opened without storage stays
/// closed). The storage must hold the window's width x height cells (debug builds assert it when it opens; a release
/// build shows the rows that fit) and stay valid while the window is open and until its last queued copy lands (the
/// next VBlank after its last update(), print(), clear() or fill()).
/// @param w      the window
/// @param tiles  the storage
void attach(Window& w, Tiles tiles);

/// Opens a window: writes its map cells and clears it.
/// @param w       the window (with its storage, attach())
/// @param config  where and how (kept by value)
void open(Window& w, const Config& config);

/// Closes a window (its map cells stay as they are: the caller redraws what was under it).
/// @param w  the window
void close(Window& w);

/// Whether a window is open.
/// @param w  the window
/// @return true while it is
bool is_open(const Window& w);

/// Clears a window and types a text.
/// @param w        the window
/// @param codes    encoded text ending with kCodeEnd (it must stay valid while it types)
/// @param instant  print it at once (prompts still wait)
void print(Window& w, const Code* codes, bool instant);

/// Clears a window (nothing types any more).
/// @param w  the window
void clear(Window& w);

/// Advances a window one frame and copies what changed to VRAM.
/// @param w        the window
/// @param pressed  Accept or Back went down this frame (goes on past a prompt, a scroll or a page break; C10:
///                 kSpeedupPerFrame: speeds the text up)
/// @param held     Accept or Back is held (types faster, Config::speedup)
void update(Window& w, bool pressed, bool held);

/// Whether a window's text still types, scrolls or waits for a button.
/// @param w  the window
/// @return true until it is printed to its end
bool busy(const Window& w);

/// Whether a window's text waits for a button (a prompt, a scroll, a page break or WaitButton).
/// @param w  the window
/// @return true while it does
bool waiting(const Window& w);

/// Draws a frame around a window's rectangle (Config x, y, width, height): its padding and its border ring; the
/// window's own cells are left as they are. Cells left of or above the layer's map are skipped.
/// @param c  the window's settings (bg, rectangle)
/// @param f  the frame
void draw_frame(const Config& c, const Frame& f);

/// Writes one tile to a window's rectangle, its padding and its border (closing a framed box).
/// @param c      the window's settings (bg, rectangle)
/// @param f      the frame (its padding)
/// @param blank  the cell written (tile and palette bank, hal::video::map_cell())
void clear_frame(const Config& c, const Frame& f, hal::video::MapCell blank);

/// N3: draws a text at a pixel position of the window at once, over what is there (nothing is cleared), in the
/// window's colors (menus print their options this way). Glyphs, colors, fonts, shifts and placeholders as print()
/// takes them; a newline goes back to @p x one line lower; waits (prompts, page breaks, pauses) are skipped.
/// The window must not be typing a text (print()) at the time.
/// @param w      the window
/// @param x      pixels from the window's left edge
/// @param y      pixels from its top edge (the glyphs' top)
/// @param codes  encoded text ending with kCodeEnd
/// @return the pen's column after the text
int print_at(Window& w, int x, int y, const Code* codes);

/// N3: fills a rectangle of the window's pixels with one palette entry (clipped to the window).
/// @param w       the window
/// @param x       left, pixels
/// @param y       top
/// @param width   pixels across
/// @param height  pixels down
/// @param entry   palette entry
void fill(Window& w, int x, int y, int width, int height, uint8_t entry);

/// C13a: draws one glyph into any buffer of 4bpp tiles laid out as a window's storage (cells row-major,
/// kWordsPerTile words each) by the window's glyph rule (see the file comment: its width's columns and glyph_height
/// rows, clipped to the buffer; 1 ink, 2 shadow, 0 and 3 background; entries 0 not written), a word at a time: for
/// screens that keep their own window buffers.
/// @param tiles         the buffer
/// @param width_cells   its cells across
/// @param height_cells  its cells down
/// @param font          the font
/// @param c             the code (one the font leaves out draws nothing)
/// @param x             left, pixels
/// @param y             top, pixels
/// @param ink           ink entry
/// @param shadow        shadow entry
/// @param background    background entry
void draw_glyph_tiles(uint32_t* tiles, int width_cells, int height_cells, const PropFont& font, Code c, int x, int y,
                      uint8_t ink, uint8_t shadow, uint8_t background);

/// C13a: fills a rectangle of such a buffer with one entry (clipped to it), a word at a time.
/// @param tiles         the buffer
/// @param width_cells   its cells across
/// @param height_cells  its cells down
/// @param x             left, pixels
/// @param y             top
/// @param width         pixels across
/// @param height        pixels down
/// @param entry         palette entry
void fill_tiles(uint32_t* tiles, int width_cells, int height_cells, int x, int y, int width, int height, uint8_t entry);

/// N3: the colors print_at() draws with until the next print() (a print() starts from the Config's).
/// @param w           the window
/// @param ink         ink entry
/// @param background  background entry (fill pixels)
/// @param shadow      shadow entry
void set_colors(Window& w, uint8_t ink, uint8_t background, uint8_t shadow);

/// Width of a text's first line in pixels (placeholders expanded; C13a: Clear, SkipTo, ClearTo and MinSpacing as
/// the handheld games count them when they measure a string; other controls take none).
/// @param font   the font
/// @param codes  encoded text
/// @return pixels
int line_width(const PropFont& font, const Code* codes);

} // namespace capsule::text::window

/// E1 (M4): the text's sound controls (the game's audio; weak defaults: nothing plays, nothing is waited for).
namespace capsule::text::sound
{

/// A sound control met while typing: Control::Se / Bgm (value: the sound number), PauseMusic, ResumeMusic (0).
/// @param c      the control
/// @param value  its 16-bit argument (low byte first), or 0
void control(Control c, uint16_t value);

/// Whether Control::WaitSe still waits (the game's sound effect plays).
/// @return true while it does
bool waiting();

} // namespace capsule::text::sound
