/// @file
/// N3 (D-32, engine B2 core): menus drawn in a text window (capsule/text/window.h): a column of options, a grid,
/// or a list that shows some of its options and scrolls, each with a cursor glyph before the chosen option. How a
/// menu looks and behaves is data (MenuStyle, a text box style's "menu" object: capsule.assets/1 kind "text_box"),
/// so a game supplies its positions, cursor, sounds and timing; what it shows is the caller's (Options).
///
/// Behaviour, frame by frame (update() once a frame with that frame's Keys, capsule/ui/keys.h):
/// - the first Options::delay frames (else MenuStyle::input_delay) read nothing;
/// - MenuStyle::order kOneAction: the first of Accept, the cancel keys, Up, Down (a grid: then Left, Right) that
///   went down acts and the frame ends there; kEveryKey: Up, Down, Accept, the cancel keys each act in that order
///   (a frame with Up and Accept moves, then picks);
/// - Accept answers the option under the cursor (MenuStyle::sound_choose); a cancel key (MenuStyle::cancel_keys)
///   answers kCancel when Options::cancel allows it (MenuStyle::sound_cancel), else nothing happens;
/// - Up / Down (MenuStyle::move_keys: on their press, or on presses and repeats) move the cursor one option; past
///   the first or the last it wraps (MenuStyle::wrap: kWrapAlways, kWrapAbove with more than wrap_above options)
///   or stays; MenuStyle::sound_move plays on every such key (kSoundAlways) or only when the cursor moved
///   (kSoundWhenMoved). A grid moves in its row or column and never wraps (a move onto a cell past the last option
///   stays put); a list never wraps;
/// - a list (Options::shown below Options::count) keeps the cursor near the middle of the shown rows: moving down,
///   the cursor goes down to the row below the middle, then the list scrolls one option and the cursor stays; at
///   the end the cursor goes on to the last row (moving up, the mirror image: down to the row above the middle).
///   A list plays MenuStyle::sound_open when it opens (and its move sound only when the selection changed).
///
/// Drawing: open() prints every shown option at (option_x + column * Options::option_width, top + row *
/// row_height) and the cursor glyph at (cursor_x + column * option_width, top + row * row_height); a move fills
/// the old cursor's cursor_width x cursor_height rectangle with the window's background and prints the glyph at the
/// new place; a list's scroll prints its shown options again. The window (and its frame) is the caller's: it opens
/// it before open() and closes it after the menu.
#pragma once

#include "capsule/core/types.h"
#include "capsule/text/encode.h"
#include "capsule/text/window.h"
#include "capsule/ui/keys.h"

#include <cstdint>

namespace capsule::ui
{

inline constexpr text::Code kNoCursor = text::kCodeEnd;   ///< MenuStyle::cursor: no cursor glyph is drawn.
inline constexpr int16_t kNothing = -1;    ///< update(): nothing was chosen this frame.
inline constexpr int16_t kCancel = -2;     ///< update(): a cancel key closed the menu.
inline constexpr int16_t kStyleDelay = -1; ///< Options::delay: the style's input_delay.
inline constexpr int kMaxItems = 64;       ///< Most options a menu holds.

/// @name MenuStyle::wrap
///@{
inline constexpr uint8_t kWrapNever = 0;    ///< The cursor stops at the first and the last option.
inline constexpr uint8_t kWrapAlways = 1;   ///< Past the last option to the first, and back.
inline constexpr uint8_t kWrapAbove = 2;    ///< Wraps when the menu has more than wrap_above options.
///@}

/// @name MenuStyle::move_keys
///@{
inline constexpr uint8_t kMovePressed = 0;    ///< Up / Down move on their press only.
inline constexpr uint8_t kMoveRepeated = 1;   ///< On their press and on the pad's repeats (capsule/ui/keys.h).
///@}

/// @name MenuStyle::move_sound
///@{
inline constexpr uint8_t kSoundWhenMoved = 0;   ///< sound_move only when the cursor moved.
inline constexpr uint8_t kSoundAlways = 1;      ///< sound_move on every Up / Down that acts.
///@}

/// @name MenuStyle::order
///@{
inline constexpr uint8_t kOneAction = 0;   ///< The first key that went down acts (Accept, cancel, Up, Down, ...).
inline constexpr uint8_t kEveryKey = 1;    ///< Up, Down, Accept, cancel each act, in that order.
///@}

inline constexpr uint16_t kNoSound = 0;   ///< A MenuStyle sound: none.

/// How a menu looks and behaves (ROM; a text box style's "menu" object). Pixels are from the window's top-left.
struct MenuStyle
{
   uint8_t option_x = 0;        ///< Left of an option's text.
   uint8_t cursor_x = 0;        ///< Left of the cursor glyph.
   uint8_t top = 0;             ///< Top of the first row.
   uint8_t row_height = 0;      ///< Pixels per row.
   uint8_t cursor_width = 0;    ///< Width of the rectangle a cursor move clears.
   uint8_t cursor_height = 0;   ///< Its height.
   text::Code cursor = kNoCursor;   ///< The cursor glyph (in the window's font), or kNoCursor.
   uint8_t wrap = kWrapNever;   ///< kWrapNever, kWrapAlways or kWrapAbove.
   uint8_t wrap_above = 0;      ///< kWrapAbove: wraps with more options than this.
   uint8_t input_delay = 0;     ///< Frames after open() before input is read.
   uint8_t move_keys = kMovePressed;      ///< kMovePressed or kMoveRepeated.
   uint8_t move_sound = kSoundWhenMoved;  ///< kSoundWhenMoved or kSoundAlways.
   uint8_t order = kOneAction;            ///< kOneAction or kEveryKey.
   hal::input::KeyBits cancel_keys = hal::input::key_bit(hal::input::KEY_B);   ///< Keys that cancel.
   uint16_t sound_move = kNoSound;     ///< Sound of a cursor move (the game's numbering, play_sound()).
   uint16_t sound_choose = kNoSound;   ///< Sound of Accept.
   uint16_t sound_cancel = kNoSound;   ///< Sound of a cancel.
   uint16_t sound_open = kNoSound;     ///< Sound when a list opens.
   AssetKey arrow_sprite{};            ///< A list's scroll arrows (capsule/ui/arrows.h): the sprite, or key 0.
   uint8_t arrow_up = 0;               ///< Its animation of the up arrow.
   uint8_t arrow_down = 0;             ///< Its animation of the down arrow.
   uint8_t arrow_amplitude = 0;        ///< Bounce: pixels at the sine's peak.
   uint8_t arrow_speed = 0;            ///< Bounce: sine steps (256 a turn) a frame (the down arrow runs backwards).
};

/// What a menu shows (the caller's, per menu).
struct Options
{
   const text::Code* const* items = nullptr;   ///< The options' texts (at least count of them).
   uint8_t count = 0;            ///< Options (at most kMaxItems).
   uint8_t cursor = 0;           ///< The option the cursor starts on (past the last: the first).
   bool cancel = true;           ///< A cancel key answers kCancel (false: it does nothing).
   uint8_t columns = 1;          ///< More than 1: a grid of columns x (count / columns) options, row by row.
   uint8_t option_width = 0;     ///< A grid: pixels per column.
   uint8_t shown = 0;            ///< A list: rows shown (0, or count or more: every option, no scrolling).
   uint8_t scroll = 0;           ///< A list: the first option shown.
   int16_t delay = kStyleDelay;  ///< Frames before input is read, or kStyleDelay.
};

/// A menu's state (plain data; the window it draws in is the caller's).
struct Menu
{
   const MenuStyle* style;             ///< How it behaves.
   text::window::Window* window;       ///< Where it draws.
   const text::Code* const* items;     ///< The options' texts.
   uint8_t count;                      ///< Options.
   uint8_t cursor;                     ///< The option under the cursor.
   uint8_t scroll;                     ///< A list: the first option shown (else 0).
   uint8_t shown;                      ///< Rows shown (a list: fewer than count).
   uint8_t columns;                    ///< Grid columns (1: a column).
   uint8_t rows;                       ///< Grid rows.
   uint8_t option_width;               ///< A grid: pixels per column.
   uint8_t delay;                      ///< Frames left before input is read.
   bool cancel;                        ///< A cancel key answers.
   bool wrap;                          ///< The cursor wraps (the style's rule for this count).
   bool open;                          ///< Shown.
};

/// Shows a menu in an open window: prints its options and the cursor (and a list plays its open sound).
/// @param m        the menu
/// @param w        the window (open; the menu draws over what is in it)
/// @param style    how it looks and behaves
/// @param options  what it shows
void open(Menu& m, text::window::Window& w, const MenuStyle& style, const Options& options);

/// One frame of a menu.
/// @param m  the menu
/// @param k  the frame's keys
/// @return the option chosen, kCancel, or kNothing
int16_t update(Menu& m, const Keys& k);

/// Moves the cursor to an option (a list scrolls to show it); nothing is played.
/// @param m       the menu
/// @param option  the option (past the last: ignored)
void move_to(Menu& m, uint8_t option);

/// Prints the menu's options and cursor again (after the caller drew over its window).
/// @param m  the menu
void redraw(Menu& m);

/// Forgets a menu (its window stays as it is: the caller closes it).
/// @param m  the menu
void close(Menu& m);

/// Whether a menu is shown.
/// @param m  the menu
/// @return true between open() and close()
bool is_open(const Menu& m);

/// The widest of some texts in a font, in pixels (placeholders expanded).
/// @param font   the font
/// @param items  the texts
/// @param count  how many
/// @return the widest's line_width()
int widest(const text::PropFont& font, const text::Code* const* items, uint8_t count);

/// Plays a menu's sound (a game with audio defines it; the engine's default plays nothing).
/// @param id  the sound (the game's numbering; never kNoSound)
void play_sound(uint16_t id);

}   // namespace capsule::ui
