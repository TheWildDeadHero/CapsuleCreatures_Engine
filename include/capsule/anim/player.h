/// @file
/// The animation-script player shell (BF, D-38): runs a bank's scripts (capsule/anim/bank.h) one command after the
/// other, with the game's command table, sprite templates, visual tasks, sound tasks and callbacks.
///
/// The engine runs the generic commands (Op: waits, jumps, calls, arguments, sprites, tasks, graphics, sound,
/// colour effects, layers); a game adds its own commands (opcodes kFirstGameOpcode-255) to its command table and
/// may replace generic ones. Each command is a function that reads its operands from the player (read_* below),
/// so the player knows nothing of a command's meaning.
///
/// Frames (frame(), once per frame while running(); docs/ANIM_SCRIPT.md has worked examples):
/// - Run mode: commands run one after the other in the same frame while frames_to_wait is 0 and the script is
///   active; a command that waits sets frames_to_wait (yield(): 1, the same command or the next one runs next frame)
///   or enters wait mode (wait_frames()).
/// - Wait mode: each frame counts frames_to_wait down by one; the frame it is 0 or less the player only leaves wait
///   mode (frames_to_wait back to 0), and commands run again the frame after. A Delay of n therefore lets n + 2
///   frames pass before the next command runs (n = 0 waits as -1: two frames).
/// - Counts: CreateSprite / CreateTask add a visual, CreateSoundTask a sound one; finish_sprite(), finish_task(),
///   finish_sound_task() take them back (the game's sprites and tasks call them when they end). WaitVisual and End
///   wait for them.
#pragma once

#include "capsule/anim/bank.h"
#include "capsule/core/vm_ext.h"
#include "capsule/screen/affine.h"
#include "capsule/screen/sprites.h"
#include "capsule/screen/tasks.h"

#include <cstdint>

namespace capsule::anim
{

inline constexpr int kCallDepth = 4;              ///< Calls a script may nest.
inline constexpr int kMaxCommandsPerFrame = 1024; ///< Commands one frame may run before the player stops (Runaway).
inline constexpr int kKinds = static_cast<int>(SymbolKind::NUM_KINDS);   ///< Symbol kinds.

struct Player;

/// A command: reads its operands (read_* below) and does its work; it must advance the program counter (by reading
/// its operands, or jump()) unless it waits (yield(), wait_frames()) or stops the player.
using Command = void (*)(Player& p);

/// A sprite template of the game's table: the sprite's template and its affine animations.
struct SpriteTemplate
{
   const screen::sprites::Template* sprite;        ///< The sprite's template.
   const screen::affine::Cmd* const* affine_anims; ///< Its affine animations, or nullptr (an affine sprite gets a
                                                   ///< matrix all the same; a plain sprite runs none).
};

/// The game's tables, one per symbol kind (an entry's index is what a bank's symbol, once bound, holds).
struct Tables
{
   const SpriteTemplate* templates;               ///< Sprite templates.
   uint16_t template_count;                        ///< Entries of templates.
   const screen::tasks::Func* tasks;               ///< Visual tasks.
   uint16_t task_count;                            ///< Entries of tasks.
   const screen::tasks::Func* sound_tasks;         ///< Sound tasks.
   uint16_t sound_task_count;                      ///< Entries of sound_tasks.
   const screen::sprites::Callback* callbacks;     ///< Sprite callbacks.
   uint16_t callback_count;                        ///< Entries of callbacks.
   const void* const* data;                        ///< Game data.
   uint16_t data_count;                            ///< Entries of data.
   const char* const* names[kKinds];               ///< Each kind's entry names, for bind() (nullptr: none).
};

/// What the game does for the generic commands (any may be nullptr: the default named with each).
struct Hooks
{
   /// CreateSprite: makes the sprite (create_sprite() helps) and returns it, or screen::sprites::kNoSprite.
   /// Default: create_sprite(t, 0, 0, flags).
   uint8_t (*create_sprite)(Player& p, const SpriteTemplate& t, uint8_t flags);
   /// End, once no visual or sound task runs: true when the script may end (false: End tries again next frame).
   /// Default: true.
   bool (*finish)(Player& p);
   /// WaitSound: true while a sound the script started still plays. Default: false.
   bool (*sound_busy)(Player& p);
   /// PlaySe: plays a sound effect at a pan (-64 left to 63 right). Default: nothing.
   void (*play_se)(Player& p, uint16_t se, int8_t pan);
   /// LoadGfx: loads a tag's sprite sheet and palette. Default: nothing.
   void (*load_gfx)(Player& p, uint16_t tag);
   /// UnloadGfx: frees a tag's sprite sheet and palette. Default: nothing.
   void (*unload_gfx)(Player& p, uint16_t tag);
};

/// A game's player setup (static storage).
struct Game
{
   const Command* commands;   ///< kOpcodes entries (nullptr entries: unknown opcodes); see generic_table().
   const Tables* tables;      ///< Its tables.
   const Hooks* hooks;        ///< Its hooks (nullptr: every default).
};

/// Why a player stopped on its own.
enum class PlayerError : uint8_t
{
   None,           ///< Running, or ended with End.
   BadScript,      ///< start() was given a script the bank does not have.
   BadOpcode,      ///< An opcode without a command.
   BadOperand,     ///< An operand past the code, a symbol of the wrong kind or out of its table, too many arguments.
   CallDepth,      ///< Call nested past kCallDepth, or Return without a Call.
   Runaway,        ///< kMaxCommandsPerFrame commands in one frame (a command that never advances).
   NUM_ERRORS      ///< Number of errors.
};

/// One running script (the game keeps one per animation it runs at once; static storage).
struct Player
{
   const Bank* bank;              ///< Its bank.
   const Game* game;              ///< The game's setup.
   const uint16_t* map;           ///< bind()'s symbol map, or nullptr (the bank is indexed).
   void* context;                 ///< The game's own pointer (the player never reads it).
   uint32_t pc;                   ///< Next byte to read (a code address).
   uint32_t command_pc;           ///< Address of the command running (or last run).
   int16_t frames_to_wait;        ///< Frames to wait (see the file comment).
   bool waiting;                  ///< In wait mode.
   bool active;                   ///< The script runs.
   PlayerError error;             ///< Why it stopped, if it stopped on its own.
   uint8_t depth;                 ///< Calls nested.
   uint8_t visual_count;          ///< Visual tasks and sprites running.
   uint8_t sound_count;           ///< Sound tasks running.
   int16_t args[kMaxArgs];        ///< The arguments the last commands set.
   uint32_t stack[kCallDepth];    ///< Return addresses.
   uint32_t frames;               ///< frame() calls since start().
   uint32_t commands;             ///< Commands run since start().
};

/// A kOpcodes-entry command table.
struct CommandTable
{
   Command at[kOpcodes];   ///< By opcode.
};

/// @name Generic commands (the handlers of Op; a game's table may use them at any opcode)
///@{

/// Op::End.
/// @param p  the player
void cmd_end(Player& p);
/// Op::Delay.
/// @param p  the player
void cmd_delay(Player& p);
/// Op::WaitVisual.
/// @param p  the player
void cmd_wait_visual(Player& p);
/// Op::WaitSound.
/// @param p  the player
void cmd_wait_sound(Player& p);
/// Op::Jump.
/// @param p  the player
void cmd_jump(Player& p);
/// Op::Call.
/// @param p  the player
void cmd_call(Player& p);
/// Op::Return.
/// @param p  the player
void cmd_return(Player& p);
/// Op::SetArg.
/// @param p  the player
void cmd_set_arg(Player& p);
/// Op::JumpIfArgEq.
/// @param p  the player
void cmd_jump_if_arg_eq(Player& p);
/// Op::CreateSprite.
/// @param p  the player
void cmd_create_sprite(Player& p);
/// Op::CreateTask.
/// @param p  the player
void cmd_create_task(Player& p);
/// Op::CreateSoundTask.
/// @param p  the player
void cmd_create_sound_task(Player& p);
/// Op::LoadGfx.
/// @param p  the player
void cmd_load_gfx(Player& p);
/// Op::UnloadGfx.
/// @param p  the player
void cmd_unload_gfx(Player& p);
/// Op::PlaySe.
/// @param p  the player
void cmd_play_se(Player& p);
/// Op::SetBlend.
/// @param p  the player
void cmd_set_blend(Player& p);
/// Op::BlendOff.
/// @param p  the player
void cmd_blend_off(Player& p);
/// Op::BgShow.
/// @param p  the player
void cmd_bg_show(Player& p);
/// Op::BgScroll.
/// @param p  the player
void cmd_bg_scroll(Player& p);
/// Op::Nop.
/// @param p  the player
void cmd_nop(Player& p);
///@}

/// The engine's command table: the generic commands at their opcodes, nothing else (a game copies it and adds its
/// own: `constexpr CommandTable kMine = with(generic_table(), 0x40, my_cmd);`).
/// @return the table
constexpr CommandTable generic_table()
{
   CommandTable t{};
   const Command generic[] = {cmd_end,       cmd_delay,       cmd_wait_visual,   cmd_wait_sound,      cmd_jump,
                              cmd_call,      cmd_return,      cmd_set_arg,       cmd_jump_if_arg_eq,  cmd_create_sprite,
                              cmd_create_task, cmd_create_sound_task, cmd_load_gfx, cmd_unload_gfx,   cmd_play_se,
                              cmd_set_blend, cmd_blend_off,   cmd_bg_show,       cmd_bg_scroll,       cmd_nop};
   static_assert(sizeof(generic) / sizeof(generic[0]) == static_cast<int>(Op::NUM_OPS));
   for (int i = 0; i < static_cast<int>(Op::NUM_OPS); ++i)
   {
      t.at[i] = generic[i];
   }
   return t;
}

/// A command table with one entry replaced.
/// @param t       the table
/// @param opcode  the opcode
/// @param c       its command (nullptr: unknown)
/// @return the new table
constexpr CommandTable with(CommandTable t, uint8_t opcode, Command c)
{
   t.at[opcode] = c;
   return t;
}

/// Resolves a bank's symbols against the game's tables by name (host tools and banks written without indices).
/// @param b      the bank
/// @param t      the tables (their names)
/// @param[out] map  symbol_count entries: each symbol's index in its kind's table (kUnbound when not found)
/// @return symbols not found (0: every one bound)
uint16_t bind(const Bank& b, const Tables& t, uint16_t* map);

/// Starts a script: arguments cleared, counts and calls cleared, run mode (the first commands run at the next
/// frame()).
/// @param p       the player
/// @param b       the bank
/// @param g       the game's setup
/// @param script  the script's index
/// @param map     bind()'s map, or nullptr when the bank is indexed
void start(Player& p, const Bank& b, const Game& g, uint16_t script, const uint16_t* map = nullptr);

/// Stops a script where it is (its sprites and tasks stay).
/// @param p  the player
void stop(Player& p);

/// One frame (see the file comment).
/// @param p  the player
void frame(Player& p);

/// Whether a script runs.
/// @param p  the player
/// @return true until End (or an error) stops it
bool running(const Player& p);

/// @name Reading operands (commands call these in their signature's order)
///@{

/// Reads a u8.
/// @param p  the player
/// @return the value
uint8_t read_u8(Player& p);

/// Reads an s8.
/// @param p  the player
/// @return the value
int8_t read_s8(Player& p);

/// Reads a u16.
/// @param p  the player
/// @return the value
uint16_t read_u16(Player& p);

/// Reads an s16.
/// @param p  the player
/// @return the value
int16_t read_s16(Player& p);

/// Reads a u32.
/// @param p  the player
/// @return the value
uint32_t read_u32(Player& p);

/// Reads a code address.
/// @param p  the player
/// @return the address
uint32_t read_address(Player& p);

/// Reads a symbol and gives its index in the game's table of that kind (a symbol of another kind, or past its
/// table, stops the player with BadOperand).
/// @param p     the player
/// @param kind  the kind the operand must name
/// @return the table index (0 after an error)
uint16_t read_symbol(Player& p, SymbolKind kind);

/// Reads an argument list into Player::args (the entries past its count keep their values).
/// @param p  the player
/// @return the count
uint8_t read_args(Player& p);
///@}

/// Goes to a code address.
/// @param p        the player
/// @param address  the address
void jump(Player& p, uint32_t address);

/// Waits in wait mode (Delay's rule: n frames, 0 as -1).
/// @param p  the player
/// @param n  frames
void wait_frames(Player& p, int16_t n);

/// Waits one frame in run mode (the command runs again, or the next one, the next frame).
/// @param p  the player
void yield(Player& p);

/// Stops a player with an error.
/// @param p  the player
/// @param e  the error
void fail(Player& p, PlayerError e);

/// @name Aliases (D-39: game opcodes that expand to generic steps, capsule/core/vm_ext.h)
///@{

/// Resolves a reference an alias step reads or writes (a game variable: the manifest's refs give the numbers).
/// @param p      the player
/// @param ref    the reference (the game's own numbering)
/// @param bytes  bytes the step reads or writes there
/// @return them, or nullptr (PlayerError::BadOperand)
using Resolve = uint8_t* (*)(Player& p, uint32_t ref, uint16_t bytes);

/// A game's alias steps (ROM, generated by tools/vm_ext.py with its command table).
struct AliasSet
{
   const vm_ext::Step* steps;   ///< Every alias's steps.
   uint16_t step_count;         ///< Entries of steps.
   Resolve resolve;             ///< The references' resolver, or nullptr (no Ref arguments).
};

/// Runs an alias at the command: its steps in order, then (when every step fell through) the program counter
/// past the command. The player's steps: Nop, Jump, JumpIf, JumpIfBytes, Yield, WaitUntil (yield() until the
/// compare holds), Hold (stays without waiting, as a command that never advances), Modify, Copy; any other
/// stops it with BadOpcode. Every code-address operand is checked first (BadOperand), as the operand readers do.
/// @param p      the player (its pc on the command's opcode)
/// @param alias  the alias
/// @param set    the game's steps and resolver
void run_alias(Player& p, const vm_ext::Alias& alias, const AliasSet& set);
///@}

/// Makes a sprite of a template as CreateSprite does: the sprite created and animated at once (its callback, then
/// its animations), an affine sprite (or one with affine animations) given a matrix and its animations first.
/// @param t            the template
/// @param x            centre x
/// @param y            centre y
/// @param subpriority  lower in front
/// @return the sprite, or screen::sprites::kNoSprite
uint8_t create_sprite(const SpriteTemplate& t, int16_t x, int16_t y, uint8_t subpriority);

/// Ends a script's sprite: its matrix freed, the sprite destroyed, one visual less.
/// @param p   the player
/// @param id  the sprite
void finish_sprite(Player& p, uint8_t id);

/// Ends a script's visual task: destroyed, one visual less.
/// @param p   the player
/// @param id  the task
void finish_task(Player& p, uint8_t id);

/// Ends a script's sound task: destroyed, one sound task less.
/// @param p   the player
/// @param id  the task
void finish_sound_task(Player& p, uint8_t id);

}   // namespace capsule::anim
