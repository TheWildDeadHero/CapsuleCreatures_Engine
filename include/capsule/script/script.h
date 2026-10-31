/// @file
/// Script VM, flags and variables (B-1, B-2).
///
/// **Flags and variables** are read and written by scripts and by game code
/// through the functions here. Writes clamp to the variable's declared range
/// and add / subtract saturate (P-18). Lifetime banks are cleared by
/// new_day() and change_map(); clearing a bank is one fill.
///
/// **VM.** A Context runs one script and the scripts it calls. update()
/// executes until the script waits (text, button, menu, delay, a native that
/// is not done), ends, or has run kOpsPerFrame instructions (a script that
/// loops without waiting continues next frame instead of stalling the
/// frame). Waiting goes through the port (capsule/script/port.h); game
/// actions are natives (game::native()). Call frame() once per frame before
/// updating any context, so every context sees the same input edges.
///
/// **Field placeholders (D-22 decision 6).** placeholder_text() writes the
/// text of the bank's field placeholders (capsule/script/data.h Placeholder)
/// and has the shape of text::FieldResolver, so a game can register it with
/// text::set_field_resolver(). Record bindings read the player's party
/// through party::role_get() (capsule/script/port.h; the engine's port reads
/// the party given to party::attach()): a text role prints its codes, any
/// other role its value in decimal. A game may still register its own
/// resolver for kind Record (placeholder()) and call placeholder_text() for
/// the rest.
///
/// **Party records (D-22 decision 2).** role_get / role_set statements read
/// and write a role of a party record (engine and custom roles alike) through
/// party::role_get() / party::role_set().
///
/// **Map triggers.** enter_map() runs a map's on_transition and on_load
/// scripts at once (the compiler proves they never wait) and arms its
/// on_frame triggers; run_map_scripts() runs the kinds I4 added (on_resume,
/// on_warp_into, on_dive, on_return). poll_triggers() checks only the triggers whose flags
/// or variables changed since they were last false, so an idle map costs
/// nothing (B-1); a true trigger stays armed and fires again until the
/// state it reads changes, as the reference games' per-frame polling does.
#pragma once

#include "capsule/core/pod.h"
#include "capsule/script/data.h"
#include "capsule/text/encode.h"

#include <cstdint>

namespace capsule::script
{

/// Instructions one update() may run before it yields.
inline constexpr int kOpsPerFrame = 1000;

/// What a context is waiting for.
enum class Wait : uint8_t
{
   None,          ///< Running (or idle).
   Delay,         ///< Frames.
   Typing,        ///< Message still typing.
   Button,        ///< Accept / Back.
   YesNo,         ///< Yes / no answer.
   Choice,        ///< Menu answer.
   Native,        ///< A native returned Wait.
   Object,        ///< An object (or the player) walks.
   Moves,         ///< Routes started by scripts run (wait_moves).
   Number,        ///< Number entry answer.
   Key,           ///< A key press.
   Warp,          ///< A warp under way.
   Scroll,        ///< The camera panning.
   Bag,           ///< The bag menu (G2).
   Shop,          ///< A shop list.
   Boxes,         ///< The storage boxes menu (G3).
   Transform,     ///< A party member's transformation (G4).
   Flight,        ///< The flight menu (F2; then Warp when a point is picked).
   Combine,       ///< A combination (B1).
   Nursery,       ///< The nursery's party menu or the nickname prompt (B2).
   Door,          ///< I4: a door's animation.
   Fade,          ///< I4: a screen fade.
   Sound,         ///< I4: a sound effect, fanfare or cry.
   Mart,          ///< I4: a mart list.
   State,         ///< I4: the game is busy (game::state_busy()).
   NUM_WAITS      ///< Number of waits.
};

/// One call-stack frame.
struct Frame
{
   uint16_t script;   ///< Script.
   uint16_t pc;       ///< Offset of the next instruction in it.
};
CAPSULE_POD(Frame, 4);

/// A running script and its callers (plain data: copy it to snapshot it).
struct Context
{
   Frame stack[kMaxStack];   ///< stack[depth - 1] is the running script.
   uint32_t delay;           ///< Frames left of a Delay.
   uint16_t result_var;      ///< YesNo / Choice / Native: where the answer goes.
   uint8_t depth;            ///< Frames in use; 0 = idle.
   Wait wait;                ///< What it waits for.
   uint8_t cancel;           ///< Choice: index Back selects, or kNoCancel.
   uint8_t self;             ///< Map object the script belongs to (kObjectSelf), or kNoSelf.
   uint8_t last_object;      ///< I4: the object the script last gave a route (kObjectLast), or kNoSelf.
   uint8_t reserved;         ///< 0.
};

/// @name Flags and variables
///@{

/// A flag's value.
/// @param f  the flag
/// @return set or not
bool flag(FlagId f);
/// Sets or clears a flag.
/// @param f   the flag
/// @param on  set
void set_flag(FlagId f, bool on);
/// A variable's value.
/// @param v  the variable
/// @return its value
int32_t var(VarId v);
/// Assigns a variable, clamped to its declared range.
/// @param v      the variable
/// @param value  the value
void set_var(VarId v, int32_t value);
/// Adds to a variable, saturating at its range.
/// @param v      the variable
/// @param delta  amount (negative subtracts)
void add_var(VarId v, int32_t delta);
/// The lifetime bank of a flag.
/// @param f  the flag
/// @return its bank
Lifetime lifetime_of(FlagId f);
/// Clears one lifetime bank (flags and variables).
/// @param l  the bank
void clear(Lifetime l);
/// Clears every bank and empties the name slots (new game).
void clear_all();
/// Day rollover: clears the daily bank.
void new_day();
/// Map change: clears the map bank, and the area bank when the area changes
/// (an interior's area root is its outdoor parent; B-2).
/// @param from_area  area root of the map being left
/// @param to_area    area root of the map being entered
void change_map(uint16_t from_area, uint16_t to_area);
/// Counts changes of flags, variables and self switches, so map object
/// pages (D-17) are chosen again only when something changed.
/// @return a counter that changes on every change
uint32_t state_version();

/// Marks state as changed (self switches, kept outside the banks).
void bump_state();

/// Bytes every bank's flags and variables, and the name slots, take in a save.
/// @return bytes
uint32_t save_size();
///@}

/// @name Field placeholders
///@{

/// A field placeholder of the bank.
/// @param index  declaration index (script_ids.h PLACEHOLDER_...)
/// @return it, or nullptr past the last
const Placeholder* placeholder(uint8_t index);
/// Writes a field placeholder's text (a text::FieldResolver): a name slot's
/// codes, a variable's value in decimal (code page digits) or a term's text;
/// nothing for a record field (the game resolves those) or an unknown index.
/// @param index     declaration index
/// @param out       where the glyph codes go
/// @param capacity  most codes to write
/// @return codes written (no kCodeEnd)
int placeholder_text(uint8_t index, text::Code* out, int capacity);
/// Sets a name slot (a placeholder bound to a name, e.g. the player's name
/// from a naming screen); longer text is cut at the slot's capacity.
/// Asserts for a placeholder that is not a name slot.
/// @param index  declaration index
/// @param codes  encoded text ending with kCodeEnd (glyphs only)
void set_name(uint8_t index, const text::Code* codes);
///@}

/// @name VM
///@{

/// Advances the port one frame (input edges, typing). Call once per frame,
/// before the update() calls of every context.
void frame();
/// Starts a script in a context (replacing whatever it ran).
/// @param c     the context
/// @param s     the script
/// @param self  the map object it belongs to (field::FieldEvent::object), or kNoSelf
void start(Context& c, ScriptId s, uint8_t self = kNoSelf);
/// Runs a context for this frame (after frame()).
/// @param c  the context
/// @return true while it is still running
bool update(Context& c);
/// Whether a context runs a script.
/// @param c  the context
/// @return true when running or waiting
inline bool running(const Context& c)
{
   return c.depth != 0;
}
/// Stops a context.
/// @param c  the context
void stop(Context& c);
/// Runs a script that never waits to its end (on_transition, on_load,
/// conditions). The compiler proves such scripts never wait, trusting each
/// native's declaration: a native declared without `waits` that returns Wait
/// here is an error (ENGINE_ASSERT); with assertions compiled out the native
/// is called again until it is done, at most 65,536 instructions in all.
/// @param s  the script
void run_now(ScriptId s);
/// I4: whether the VM runs a script that must not wait (run_now(), map
/// scripts, trigger conditions): a native that would show something over
/// frames does it at once or not at all then.
/// @return true while one runs
bool immediate();

/// N1 (D-32): the statement after the native the VM is running (its script
/// and offset): a game keeps it to continue the script there later
/// (native_goto() from another native of the same context, at the same call
/// depth). Call only from game::native().
/// @return the frame
Frame native_return_point();

/// N1 (D-32): the native the VM is running continues its script elsewhere:
/// once it returns Done, the running frame is replaced by @p to (a goto:
/// the call stack keeps its depth) instead of going on after the native. A
/// native that returns Wait drops the request. Call only from game::native().
/// @param to  where the frame goes on (a native_return_point() of this context)
void native_goto(Frame to);

/// N1 (D-32): native_goto() to the start of a script.
/// @param s  the script
void native_jump(ScriptId s);
///@}

/// @name Map triggers
///@{

/// Enters a map: runs its on_transition and on_load scripts and arms its
/// on_frame triggers (none when the map has no scripts).
/// @param map  the map's asset key
void enter_map(AssetKey map);
/// C11: enter_map() for a map a saved game resumes on: its on_load scripts only (the reference games'
/// InitMapFromSavedGame runs no on_transition), then its on_frame triggers armed as enter_map() arms them.
/// @param map  the map's asset key
void enter_saved_map(AssetKey map);
/// Checks the armed on_frame triggers whose state changed. Call while no
/// script runs on the field.
/// @param out  set to the script to start when one fires
/// @return true when a trigger fired
bool poll_triggers(ScriptId& out);
/// Drops the armed triggers (leaving a map without entering another).
void disarm_triggers();
/// I4: runs a map's scripts of a kind that runs once and never waits: every
/// OnResume, OnDive or OnReturn entry in order, or the first OnWarpInto entry
/// whose condition holds (a map script table). field::enter() runs
/// OnResume before spawning the objects and OnWarpInto after (C10),
/// field::shift_map() OnResume before spawning; a game runs OnDive and
/// OnReturn where it has them.
/// @param map   the map's asset key
/// @param kind  OnResume, OnWarpInto, OnDive or OnReturn
/// @return true when a script ran
bool run_map_scripts(AssetKey map, TriggerKind kind);
///@}

} // namespace capsule::script
