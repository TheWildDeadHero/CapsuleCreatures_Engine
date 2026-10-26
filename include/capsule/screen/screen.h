/// @file
/// Full-screen scenes (UC, D-33): screens entered from the field, from scripts and from each other, each with its own
/// background plan (capsule/screen/layers.h), sprites (capsule/screen/sprites.h), palettes, windows (text::window,
/// mirrored into a layer with layers::put_window()), tasks (capsule/screen/tasks.h) and fades (capsule/screen/
/// fade.h), on a stack (a party list opens a summary; closing the summary sets the party list up again).
///
/// The frame (frame(), once per frame while active(); the game's main loop calls it in place of the field's step,
/// then the platform's frame end, i.e. the vertical blank): the keys the game read for the frame, then the top
/// screen's input function (the reference games' first callback), then its steps in its order (the second callback:
/// tasks, sprite animation, OAM, scheduled tilemap copies, the palette fade, its own main function), then its
/// vertical blank function (work that lands with the frame). Phases of a screen:
/// - Opening (push() with Screen::open): the open fade starts at once and runs over what is below (the field: Host::
///   frame, then the fade's call; a screen below: its steps). The frame after the fade's last call is the hand-over
///   frame (what is below still runs it); the frame after that, what is below gives up its video memory
///   (Host::suspend, or the screen below's leave(true)) and the new screen's setup makes its first call. push_now()
///   makes the next frame that one.
/// - Setup: Screen::setup once a frame (it may do several of its own steps in one call) until it returns true; then
///   Screen::in starts in the same frame, and the screen runs from the next frame.
/// - Running: input, steps, vertical blank function.
/// - Closing (close()): Screen::out starts at once; input stops, the steps go on; the frame after its last call the
///   screen leaves (Screen::leave(false), nothing else runs), and the frame after that what is below resumes: a
///   screen's setup again with resumed = true (then its in fade), or the field (Host::resume once a frame until it
///   returns true; the field's own frame runs from the frame after that). These are the frames the reference games
///   take: a task sees the fade finished, the next frame's callback starts the other screen.
/// close_now() and push_now() skip the fades (a screen that runs its own fades and then calls them); a screen may
/// close() and push() another screen in one (replace(), or replace_now() without the out fade). C13a (R9 H1): the
/// frame close_now() is called in counts as close()'s frame after the fade: the screen leaves at that frame's end
/// (after the rest of its steps) and what is below resumes the next frame, so both close in the reference games'
/// frames (the frame a task calls SetMainCallback2, then the next callback's first frame). A game whose own return
/// takes more callbacks (a callback that only sets the next one) models them in its Host::resume or setup.
///
/// The field: the default host (default_host()) gives up and restores what this engine's field owns, as the reference
/// games' return to the field restores theirs without loading the map again: suspend() keeps the palettes' base
/// colours (gfx::palette::save()) and suspends the map (map::suspend()); resume() takes the reference games' four
/// steps, one a frame (state 0: the game's objects and scripts, Host::on_return; 1: the view's graphics:
/// restore_field_graphics(): the screen's layers and sprites forgotten, the map resumed with its animations
/// restarted, gfx::sprites' tiles put back, the base colours given back, blend / windows / mosaic off; 2: the game's
/// field callback until it returns true, Host::field_callback; 3: done). A game supplies its own Host built on these
/// (its message box tiles, its tasks, its fade-in).
///
/// Memory (UM): a screen's large state comes from the screen arena (capsule/screen/arena.h) while it is open. A block
/// taken while a screen is the top one is its own; when the screen leaves for good (its pop: after close(), close_now()
/// or replace()) the arena goes back to where it stood at that screen's first setup call. A screen under a child keeps
/// its blocks unless it gives them back in its leave(true) (the reference games' screens that rebuild on return).
#pragma once

#include "capsule/screen/fade.h"
#include "capsule/screen/layers.h"
#include "capsule/ui/keys.h"

#include <cstdint>

/// Full-screen scenes.
namespace capsule::screen
{

/// One step of a screen's frame (its order is the screen's data).
enum class Step : uint8_t
{
   Tasks,          ///< tasks::run().
   Animate,        ///< sprites::animate().
   BuildOam,       ///< sprites::build_oam().
   CopyTilemaps,   ///< layers::copy_scheduled().
   Fade,           ///< fade::update().
   Main,           ///< Screen::main.
   NUM_STEPS       ///< Number of steps.
};

/// The reference games' most common screen frame: tasks, sprites, OAM, tilemap copies, the palette fade.
inline constexpr Step kDefaultSteps[] = {Step::Tasks, Step::Animate, Step::BuildOam, Step::CopyTilemaps, Step::Fade};
/// Steps of kDefaultSteps.
inline constexpr uint8_t kDefaultStepCount = sizeof(kDefaultSteps) / sizeof(kDefaultSteps[0]);

inline constexpr int kMaxDepth = 4;    ///< Screens on the stack at once.
inline constexpr int kMaxSteps = 8;    ///< Steps of one screen's frame.

/// A fade a transition runs (data).
struct Transition
{
   bool on;                    ///< Run it (false: none).
   fade::Fade fade;            ///< The fade.
   fade::FadeStyle style;      ///< How it steps.
};

/// No transition.
inline constexpr Transition kNoTransition = {false, fade::kToBlack, fade::kHandheldFade};
/// A fade to black over every bank.
inline constexpr Transition kFadeToBlack = {true, fade::kToBlack, fade::kHandheldFade};
/// A fade from black over every bank.
inline constexpr Transition kFadeFromBlack = {true, fade::kFromBlack, fade::kHandheldFade};

/// A screen: its data and functions (any function may be nullptr).
struct Screen
{
   const layers::Plan* plan;                                  ///< Applied before its setup (nullptr: none).
   bool (*setup)(uint8_t& state, uint8_t arg, bool resumed);  ///< Once a frame until true (state starts at 0).
   void (*input)();                                           ///< The first callback, before the steps.
   void (*main)();                                            ///< Step::Main.
   void (*vblank)();                                          ///< After the steps.
   void (*leave)(bool for_child);                             ///< Its video memory goes (a child opens: keep state).
   const Step* steps;                                         ///< Its frame's order, or nullptr (kDefaultSteps).
   uint8_t step_count;                                        ///< Entries of steps.
   Transition open;                                           ///< Over what is below, before the setup.
   Transition in;                                             ///< After the setup.
   Transition out;                                            ///< Before it leaves.
};

/// What the field does around screens (a game's functions; any may be nullptr).
struct Host
{
   void (*suspend)();                  ///< The field gives up its video memory.
   bool (*resume)(uint8_t& state);     ///< Once a frame until true: the field is back.
   void (*frame)();                    ///< The field's frame while an open fade runs over it.
   void (*on_return)();                ///< default_host(): resume state 0 (the game's objects, scripts).
   bool (*field_callback)();           ///< default_host(): resume state 2, once a frame until true.
};

/// A screen's phase.
enum class Phase : uint8_t
{
   Opening,     ///< Its open fade runs over what is below.
   Handover,    ///< What is below gave up its video memory; its setup starts next frame.
   Setup,       ///< Its setup runs.
   Running,     ///< It runs.
   Closing,     ///< Its out fade runs.
   Left,        ///< It left; what is below resumes next frame.
   Resuming,    ///< It is set up again (resumed) after the screen above it closed.
   NUM_PHASES   ///< Number of phases.
};

/// The engine's field host: the base colours kept, the map suspended; on return the reference games' four steps
/// (see the file comment), with the game's on_return and field_callback when set_host() gave them.
/// @return it
const Host& default_host();

/// Sets the field host (the default until then: default_host()).
/// @param h  the game's host (kept by pointer; static storage)
void set_host(const Host& h);

/// The engine's part of giving the field its video memory back (default_host() resume state 1): the screen layers
/// and sprites forgotten (all hidden), the map resumed (its animations restarted), gfx::sprites' tiles put back,
/// the base colours saved at the suspend given back, blending, windows and mosaic off.
void restore_field_graphics();

/// Opens a screen over the field or over the top screen.
/// @param s    the screen (static storage)
/// @param arg  its argument (its setup's)
/// @return false when kMaxDepth screens are open
bool push(const Screen& s, uint8_t arg = 0);

/// Opens a screen without its open fade (the caller faded already).
/// @param s    the screen
/// @param arg  its argument
/// @return false when kMaxDepth screens are open
bool push_now(const Screen& s, uint8_t arg = 0);

/// Closes the top screen (its out fade first).
void close();

/// Closes the top screen without its out fade (it faded itself). C13a: called during frame(), the screen leaves at
/// the end of that frame (its remaining steps run first); called between frames, it leaves at once. What is below
/// (or replace_now()'s screen) takes over the next frame.
void close_now();

/// Closes the top screen and opens another in its place (the closed one's out fade, then the new one's setup; what
/// is below stays given up).
/// @param s    the new screen
/// @param arg  its argument
void replace(const Screen& s, uint8_t arg = 0);

/// C13a: replace() without the closed one's out fade (it faded itself): it leaves as close_now() does and the new
/// screen's setup makes its first call the next frame.
/// @param s    the new screen
/// @param arg  its argument
void replace_now(const Screen& s, uint8_t arg = 0);

/// One frame (call while active()).
/// @param keys  the frame's keys (the game's ui::read())
void frame(const ui::Keys& keys);

/// Whether a screen is open (or still handing back to the field).
/// @return true until the field is back
bool active();

/// Screens on the stack.
/// @return 0 to kMaxDepth
int depth();

/// The top screen.
/// @return it, or nullptr
const Screen* top();

/// The top screen's phase.
/// @return it (Phase::NUM_PHASES when none is open)
Phase phase();

/// The frame's keys (as given to frame()).
/// @return them
const ui::Keys& keys();

/// Frames frame() ran since the stack was last empty (tests).
/// @return the count
uint32_t frames();

/// Forgets every screen (tests, and a game's boot handing the display to the field; the field is not resumed) and
/// gives back every screen arena block (arena::reset(): the blocks field code took too).
void reset();

}   // namespace capsule::screen
