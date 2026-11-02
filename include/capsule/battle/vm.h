/// @file
/// The battle script machine (M5 BX): a bytecode interpreter whose command set belongs to the game. Scripts live in
/// banks (read-only data, usually in ROM); the game supplies the table of command functions; the machine keeps the
/// instruction position, a call stack, a frame counter for timed waits, and waits while its controllers run
/// (capsule/battle/controller.h).
///
/// A bank (all numbers little-endian, every offset in bytes):
///
/// | Offset | Size | Field |
/// |---|---|---|
/// | 0 | 4 | magic kBankMagic ("CBVM") |
/// | 4 | 2 | version kBankVersion |
/// | 6 | 2 | label count |
/// | 8 | 4 | code offset (from the bank's start) |
/// | 12 | 4 | code size |
/// | 16 | 4 | label table offset: one u32 per label, its code offset |
/// | 20 | 4 | name table offset, or 0: one u32 per label, the bank offset of its name (NUL-terminated ASCII) |
///
/// The code is a stream of commands: a command id (one byte, the index into the game's table) and the operands the
/// command reads itself, little-endian and unaligned. A pointer operand is a reference (Ref, 4 bytes): kind
/// RefKind::Code is a code offset (a jump or call target, or data inside the code); the other kinds name the
/// game's own variables and tables by id and byte offset, so a bank never holds an address. The writer is
/// tools/script_bank.py (its library and command line).
///
/// Running: step() runs the command at the instruction position unless the machine is not running, or waits on
/// its controllers (a non-zero exec mask: Result::Blocked, nothing runs). A command reads its operands (u8_at()
/// and friends: relative to its own id byte), does its work and moves the position (advance(), jump(), call(),
/// ret(), end()), or leaves it where it is to run again on the next step (a wait: pause() counts frames).
/// A game that runs one command a frame calls step() once a frame; run() repeats step() until a command yields,
/// the machine stops or a step limit is reached (an interpreter that runs many commands in one call).
#pragma once

#include "capsule/core/vm_ext.h"

#include <cstdint>

/// The battle script machine.
namespace capsule::battle::vm
{

inline constexpr uint32_t kBankMagic = 0x4D564243u;   ///< "CBVM" read as a little-endian u32.
inline constexpr uint16_t kBankVersion = 1;            ///< The bank format's version.
inline constexpr uint32_t kHeaderBytes = 24;           ///< Bytes of a bank's header.
inline constexpr uint8_t kStackDepth = 8;              ///< Return positions the call stack holds.
inline constexpr uint16_t kNoLabel = 0xFFFF;           ///< label_at(): no label there.

/// @name References (pointer operands)
///@{

/// What a reference names (its top four bits).
enum class RefKind : uint8_t
{
   Code = 0,       ///< A code offset (bits 0-27).
   Variable = 1,   ///< A game variable: id (bits 16-27) and byte offset (bits 0-15).
   Table = 2,      ///< A game table: id (bits 16-27) and byte offset (bits 0-15).
   Value = 3,      ///< An immediate value (bits 0-27).
   NUM_REF_KINDS   ///< Number of kinds the machine names (kinds 4-15 are the game's).
};

/// A reference as it is stored: kind, id, offset in one word.
using Ref = uint32_t;

inline constexpr int kRefKindShift = 28;              ///< The kind's bits start here.
inline constexpr int kRefIdShift = 16;                ///< A variable or table id's bits start here.
inline constexpr uint32_t kRefIdMask = 0xFFFu;        ///< A variable or table id's bits (12).
inline constexpr uint32_t kRefOffsetMask = 0xFFFFu;   ///< A byte offset's bits (16).
inline constexpr uint32_t kRefPayloadMask = 0x0FFFFFFFu;   ///< Everything below the kind (a code offset, a value).

/// A reference's kind.
/// @param r  the reference
/// @return its kind (game kinds are returned as their number)
constexpr RefKind ref_kind(Ref r)
{
   return static_cast<RefKind>(r >> kRefKindShift);
}

/// A variable or table reference's id.
/// @param r  the reference
/// @return 0-4095
constexpr uint16_t ref_id(Ref r)
{
   return static_cast<uint16_t>((r >> kRefIdShift) & kRefIdMask);
}

/// A variable or table reference's byte offset.
/// @param r  the reference
/// @return 0-65535
constexpr uint16_t ref_offset(Ref r)
{
   return static_cast<uint16_t>(r & kRefOffsetMask);
}

/// A code or value reference's payload.
/// @param r  the reference
/// @return 0 to 2^28 - 1
constexpr uint32_t ref_payload(Ref r)
{
   return r & kRefPayloadMask;
}

/// Builds a variable or table reference (the writer's encoding).
/// @param kind    the kind
/// @param id      0-4095
/// @param offset  0-65535
/// @return the reference
constexpr Ref make_ref(RefKind kind, uint16_t id, uint16_t offset)
{
   return (static_cast<uint32_t>(kind) << kRefKindShift) | ((static_cast<uint32_t>(id) & kRefIdMask) << kRefIdShift) |
          offset;
}

/// Builds a code reference.
/// @param offset  the code offset (below 2^28)
/// @return the reference
constexpr Ref code_ref(uint32_t offset)
{
   return offset & kRefPayloadMask;
}
///@}

/// A bank opened for reading (a view: the bytes stay where they are).
struct Bank
{
   const uint8_t* data;      ///< The whole bank.
   uint32_t size;            ///< Its bytes.
   const uint8_t* code;      ///< Its code.
   uint32_t code_bytes;      ///< The code's bytes.
   const uint8_t* labels;    ///< The label table (label_count u32 code offsets).
   const uint8_t* names;     ///< The name table, or nullptr.
   uint16_t label_count;     ///< Labels.
};

/// Why a bank was refused (open()).
enum class BankError : uint8_t
{
   None,          ///< Opened.
   TooSmall,      ///< Shorter than its header.
   BadMagic,      ///< Not a bank.
   BadVersion,    ///< Another format version.
   BadLayout,     ///< A table or the code lies outside the bank.
   NUM_BANK_ERRORS   ///< Number of values.
};

/// Opens a bank.
/// @param bank  receives the view (left empty on an error)
/// @param data  the bank's bytes (kept by pointer)
/// @param size  their count
/// @return BankError::None, or why it was refused
BankError open(Bank& bank, const uint8_t* data, uint32_t size);

/// A label's code offset.
/// @param bank  the bank
/// @param id    the label (0 to label_count - 1)
/// @return its code offset (0 for an id out of range)
uint32_t label(const Bank& bank, uint16_t id);

/// A label's name.
/// @param bank  the bank
/// @param id    the label
/// @return its name, or nullptr (no name table, or an id out of range)
const char* label_name(const Bank& bank, uint16_t id);

/// The label at a code offset (the lowest id when several share it).
/// @param bank    the bank
/// @param offset  a code offset
/// @return its id, or kNoLabel
uint16_t label_at(const Bank& bank, uint32_t offset);

/// The label at or before a code offset (the nearest one: where a position is, for traces).
/// @param bank    the bank
/// @param offset  a code offset
/// @return its id, or kNoLabel (no label at or before it)
uint16_t label_before(const Bank& bank, uint32_t offset);

/// What the machine is doing.
enum class Status : uint8_t
{
   Idle,      ///< Not started (or stopped by stop()).
   Running,   ///< Runs commands.
   Ended,     ///< A command called end().
   Fault,     ///< A command read past the code, jumped outside it, or the stack over- or underflowed.
   NUM_STATUSES   ///< Number of values.
};

/// Why the machine faulted.
enum class Fault : uint8_t
{
   None,            ///< No fault.
   OutOfCode,       ///< The position or an operand lies outside the code.
   StackOverflow,   ///< call() / push() with a full stack.
   StackUnderflow,  ///< ret() / pop() with an empty stack.
   NoCommand,       ///< An id past the table with no unknown-command function.
   BadRef,          ///< An alias step's reference resolved to nothing (run_alias()).
   BadStep,         ///< An alias step the machine does not run, or past its step table (run_alias()).
   NUM_FAULTS       ///< Number of values.
};

/// The machine (plain data: the game keeps it with its state).
struct Machine
{
   const Bank* bank;                 ///< The bank it runs.
   uint32_t ip;                      ///< The instruction position (a code offset).
   uint32_t stack[kStackDepth];      ///< Return positions.
   uint8_t depth;                    ///< Entries of stack.
   Status status;                    ///< What it is doing.
   Fault fault;                      ///< Why it faulted.
   bool yielded;                     ///< The last command called yield() (run() stops).
   uint16_t pause_frames;            ///< pause()'s counter.
   const uint32_t* waits_on;         ///< The exec mask it waits on, or nullptr (never waits).
   uint32_t wait_mask;               ///< The mask's bits that block it.
};

/// A command: reads its operands from the machine, does its work, moves the position.
/// @param m        the machine
/// @param context  the game's (CommandTable's caller passes it through)
using Command = void (*)(Machine& m, void* context);

/// Called before each command (traces, parity recorders), or nullptr.
/// @param m        the machine (its position is the command's)
/// @param id       the command id
/// @param context  the game's
using Trace = void (*)(const Machine& m, uint8_t id, void* context);

/// The game's command set.
struct CommandTable
{
   const Command* commands;   ///< Indexed by command id.
   uint16_t count;            ///< Entries of commands (ids at or past it are unknown).
   Command unknown;           ///< Runs an unknown id (nullptr: Fault::NoCommand).
   Trace trace;               ///< Before each command, or nullptr.
};

/// What a step did.
enum class Result : uint8_t
{
   Ran,       ///< A command ran.
   Blocked,   ///< Its controllers are busy: nothing ran.
   Stopped,   ///< The machine is not running (Idle, Ended or Fault): nothing ran.
   NUM_RESULTS   ///< Number of values.
};

/// Makes a machine ready (not running), waiting on nothing.
/// @param m     the machine
/// @param bank  the bank it will run (kept by pointer)
void reset(Machine& m, const Bank& bank);

/// Makes the machine wait on an exec mask: step() runs nothing while (*flags & mask) != 0.
/// @param m      the machine
/// @param flags  the mask word (kept by pointer), or nullptr (never wait)
/// @param mask   the bits that block (all bits: any controller busy)
void wait_on(Machine& m, const uint32_t* flags, uint32_t mask = ~0u);

/// Starts the machine at a code offset (the stack emptied, the pause counter cleared).
/// @param m       the machine
/// @param offset  a code offset
void start(Machine& m, uint32_t offset);

/// Starts the machine at a label.
/// @param m   the machine
/// @param id  the label
void start_label(Machine& m, uint16_t id);

/// Stops the machine (Status::Idle) without running anything more.
/// @param m  the machine
void stop(Machine& m);

/// Whether its controllers block it now.
/// @param m  the machine
/// @return true while the exec mask has blocking bits
bool blocked(const Machine& m);

/// Runs the command at the position, unless the machine is blocked or not running.
/// @param m        the machine
/// @param table    the game's commands
/// @param context  passed to the command
/// @return what it did
Result step(Machine& m, const CommandTable& table, void* context);

/// Steps until a command yields, the machine blocks or stops, or @p limit commands ran.
/// @param m        the machine
/// @param table    the game's commands
/// @param context  passed to the commands
/// @param limit    the most commands to run
/// @return commands run
uint16_t run(Machine& m, const CommandTable& table, void* context, uint16_t limit);

/// @name For commands: operands (offsets from the command's id byte)
///@{

/// A byte of the code.
/// @param m   the machine
/// @param at  bytes after the position
/// @return it (0 and Fault::OutOfCode outside the code)
uint8_t u8_at(Machine& m, uint32_t at);

/// A signed byte of the code.
/// @param m   the machine
/// @param at  bytes after the position
/// @return it
int8_t s8_at(Machine& m, uint32_t at);

/// A little-endian u16 of the code (unaligned).
/// @param m   the machine
/// @param at  bytes after the position
/// @return it
uint16_t u16_at(Machine& m, uint32_t at);

/// A little-endian u32 of the code (unaligned).
/// @param m   the machine
/// @param at  bytes after the position
/// @return it
uint32_t u32_at(Machine& m, uint32_t at);

/// A reference operand.
/// @param m   the machine
/// @param at  bytes after the position
/// @return it
Ref ref_at(Machine& m, uint32_t at);

/// The code itself at a code offset (data inside the code a reference names: lists, inline tables).
/// @param m       the machine
/// @param offset  a code offset
/// @param bytes   bytes the caller reads from there
/// @return them, or nullptr (Fault::OutOfCode) when they lie outside the code
const uint8_t* code_bytes(Machine& m, uint32_t offset, uint32_t bytes);
///@}

/// @name For commands: control
///@{

/// Moves the position past the command.
/// @param m      the machine
/// @param bytes  the command's size (its id and operands)
void advance(Machine& m, uint32_t bytes);

/// Moves the position to a code offset.
/// @param m       the machine
/// @param offset  the target (a Code reference's payload)
void jump(Machine& m, uint32_t offset);

/// Pushes a return position and jumps.
/// @param m        the machine
/// @param offset   the target
/// @param resume   the code offset ret() goes back to
void call(Machine& m, uint32_t offset, uint32_t resume);

/// Pushes a position (a command that saves where to go on).
/// @param m       the machine
/// @param offset  a code offset
void push(Machine& m, uint32_t offset);

/// Pops a position without jumping.
/// @param m  the machine
/// @return it (0 and Fault::StackUnderflow when empty)
uint32_t pop(Machine& m);

/// Pops a position and jumps there.
/// @param m  the machine
void ret(Machine& m);

/// Ends the script (Status::Ended).
/// @param m  the machine
void end(Machine& m);

/// Makes run() return after this command (the position is the command's to set).
/// @param m  the machine
void yield(Machine& m);

/// A timed wait: counts one frame per call; true once @p frames calls have counted (then the counter restarts).
/// A command that waits calls it each time it runs and advances only when it returns true.
/// @param m       the machine
/// @param frames  frames to wait (0: done at once)
/// @return true when the wait is over
bool pause(Machine& m, uint16_t frames);
///@}

/// @name Aliases (D-39: a game's opcodes that expand to generic steps, capsule/core/vm_ext.h)
///@{

/// Resolves a reference an alias step reads or writes (the game's variables and tables).
/// @param m        the machine
/// @param ref      the reference (any kind; Code references may be left to the machine: return nullptr and none
///                 is resolved)
/// @param bytes    bytes the step reads or writes there
/// @param context  the game's (step()'s)
/// @return them, or nullptr (Fault::BadRef)
using Resolve = uint8_t* (*)(Machine& m, Ref ref, uint16_t bytes, void* context);

/// A game's alias steps (ROM, generated by tools/vm_ext.py with its dispatch).
struct AliasSet
{
   const vm_ext::Step* steps;   ///< Every alias's steps.
   uint16_t step_count;         ///< Entries of steps.
   Resolve resolve;             ///< The references' resolver, or nullptr (Code references only).
};

/// Runs an alias at the position: its steps in order, then (when every step fell through) advance() by its size.
/// The machine's steps are every vm_ext::Op: Jump / JumpIf / JumpIfBytes / Call go to Code operands (a reference's
/// payload), Call pushes the position after the command, Return pops, Pause and PauseIfSet count frames with
/// pause(), WaitUntil and Hold stay on the command (WaitUntil yields), Modify and Copy read and write through
/// the resolver. Faults: OutOfCode (the command or a target outside the code), BadRef, BadStep, the stack's.
/// @param m        the machine
/// @param alias    the alias (its steps' index and count, the command's size)
/// @param set      the game's steps and resolver
/// @param context  passed to the resolver
void run_alias(Machine& m, const vm_ext::Alias& alias, const AliasSet& set, void* context);
///@}

}   // namespace capsule::battle::vm
