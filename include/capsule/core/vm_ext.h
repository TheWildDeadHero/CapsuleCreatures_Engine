/// @file
/// The generic operations a game's script-machine opcodes expand to (D-39: "aliases", data only).
///
/// A game describes its own opcodes in an extension manifest (capsule.vm-ext/1, docs/VM_EXT.md). An opcode whose
/// behaviour is pure control flow, reads and writes of game variables named by references, compares or plain waits
/// is an **alias**: a short fixed sequence of the generic steps below, with the opcode's operand layout given as
/// data. tools/vm_ext.py turns the manifest into ROM tables of steps and a small dispatch function per alias, which
/// calls the VM's run_alias() (capsule/battle/vm.h, capsule/anim/player.h). Only an opcode no generic step can
/// express stays a native function, and the manifest says why.
///
/// A step reads its arguments from the opcode's operands (at a byte offset from the opcode byte, with a type) or
/// from constants, so the same step serves every layout. Steps run in order on the command's position: a step that
/// moves the position or waits ends the sequence (Flow::Done); when every step falls through (Flow::Next) the VM
/// moves past the command (the alias's size). Every table here is const: aliases cost ROM only, never RAM.
#pragma once

#include <cstdint>

/// The generic operation vocabulary shared by the engine's VMs.
namespace capsule::vm_ext
{

/// A generic step (each VM runs the ones docs/VM_EXT.md lists for it; any other faults the VM).
enum class Op : uint8_t
{
   Nop,           ///< Nothing; falls through.
   Jump,          ///< args[0] target: goes there.
   JumpIf,        ///< args[0] compare, [1] ref, [2] value, [3] target: goes there when *ref (width bytes) compares
                  ///< true with value; else falls through.
   JumpIfBytes,   ///< args[0] ref a, [1] ref b, [2] size, [3] target: mode Equal goes there when the size bytes
                  ///< match, NotEqual when they differ; else falls through.
   Call,          ///< args[0] target: pushes the position after the command, goes there.
   Return,        ///< Pops a position and goes there.
   End,           ///< Ends the script.
   Yield,         ///< Makes the VM's run loop return after this command; falls through.
   Pause,         ///< args[0] frames: waits (the VM's frame counter); falls through once the frames have passed.
   PauseIfSet,    ///< args[0] flag ref (a byte), [1] frames: falls through when the flag is clear; when it is set,
                  ///< waits the frames, then clears it and falls through.
   WaitUntil,     ///< args[0] compare, [1] ref, [2] value: waits a frame (the command runs again) until the compare
                  ///< holds; then falls through.
   Hold,          ///< Stays on the command without waiting (a command that never advances).
   Modify,        ///< args[0] ref, [1] value: *ref = *ref (alu mode) value, width bytes; falls through.
   Copy,          ///< args[0] dest ref, [1] src ref, [2] size: copies size bytes; falls through.
   NUM_OPS        ///< Number of steps.
};

/// A comparison (JumpIf, WaitUntil): left side the variable, right side the value. Codes past the last compare
/// false.
enum class Compare : uint8_t
{
   Equal,          ///< ==
   NotEqual,       ///< !=
   Greater,        ///< > (unsigned)
   Less,           ///< < (unsigned)
   CommonBits,     ///< (a & b) != 0
   NoCommonBits,   ///< (a & b) == 0
   NUM_COMPARES    ///< Number of compares.
};

/// A read-modify-write (Modify's mode).
enum class Alu : uint8_t
{
   Set,     ///< value
   Add,     ///< old + value (wraps at the width)
   Sub,     ///< old - value (wraps at the width)
   Or,      ///< old | value
   Bic,     ///< old & ~value
   NUM_ALUS ///< Number of modes.
};

/// Where an argument comes from.
enum class From : uint8_t
{
   None,       ///< Unused (reads as 0).
   Operand,    ///< The opcode's operand at a byte offset from the opcode byte.
   Constant,   ///< Arg::value.
   NUM_FROMS   ///< Number of sources.
};

/// How an operand argument is read (the manifest's operand types, by width and meaning).
enum class Type : uint8_t
{
   U8,      ///< unsigned byte
   S8,      ///< signed byte (sign-extended)
   U16,     ///< unsigned little-endian halfword
   S16,     ///< signed halfword (sign-extended)
   U32,     ///< little-endian word
   Code,    ///< a code position (the VM's own encoding: the battle machine's Code reference, the animation
            ///< player's code address)
   Ref,     ///< a reference to game state, resolved by the game's resolver (4 bytes)
   NUM_TYPES   ///< Number of types.
};

inline constexpr int kStepArgs = 4;   ///< Arguments of a step.
inline constexpr int kArg0 = 0;       ///< A step's first argument (see Op for each step's meaning).
inline constexpr int kArg1 = 1;       ///< Its second.
inline constexpr int kArg2 = 2;       ///< Its third.
inline constexpr int kArg3 = 3;       ///< Its fourth.

/// One argument of a step.
struct Arg
{
   From from;        ///< Where it comes from.
   Type type;        ///< How an operand is read (and what a constant means: Code and Ref constants are encoded).
   uint8_t at;       ///< An operand's byte offset from the opcode byte.
   uint8_t reserved; ///< 0.
   uint32_t value;   ///< A constant.
};

/// One generic step of an alias.
struct Step
{
   Op op;                 ///< The step.
   uint8_t mode;          ///< JumpIfBytes: Compare::Equal / NotEqual; Modify: an Alu.
   uint8_t width;         ///< Bytes of the variable a ref names (JumpIf, WaitUntil, Modify: 1, 2 or 4).
   uint8_t reserved;      ///< 0.
   Arg args[kStepArgs];   ///< Its arguments (see Op).
};

/// An alias: its steps in a VM's step table, and the command's size in bytes (its opcode and operands).
struct Alias
{
   uint16_t first;   ///< Its first step's index.
   uint8_t count;    ///< Its steps (1 or more).
   uint8_t size;     ///< Bytes of the command.
};

/// What a step did.
enum class Flow : uint8_t
{
   Next,   ///< Fell through: the next step runs (after the last one the VM moves past the command).
   Done,   ///< Moved the position, waited, stopped or faulted: the command is over for now.
   NUM_FLOWS   ///< Number of values.
};

inline constexpr uint8_t kByte = 1;    ///< A byte variable.
inline constexpr uint8_t kHalf = 2;    ///< A halfword variable.
inline constexpr uint8_t kWord = 4;    ///< A word variable.
inline constexpr int kByteBits = 8;    ///< Bits of a byte.

/// A compare.
/// @param c      the compare (as stored)
/// @param left   the variable's value
/// @param right  the value
/// @return its result (false for an unknown code)
constexpr bool compare(uint8_t c, uint32_t left, uint32_t right)
{
   switch (static_cast<Compare>(c))
   {
   case Compare::Equal:
      return left == right;
   case Compare::NotEqual:
      return left != right;
   case Compare::Greater:
      return left > right;
   case Compare::Less:
      return left < right;
   case Compare::CommonBits:
      return (left & right) != 0;
   case Compare::NoCommonBits:
      return (left & right) == 0;
   case Compare::NUM_COMPARES:
      break;
   }
   return false;
}

/// A read-modify-write's new value.
/// @param a      the mode (as stored)
/// @param old    the variable's value
/// @param value  the operand
/// @return the new value (old for an unknown mode)
constexpr uint32_t apply(uint8_t a, uint32_t old, uint32_t value)
{
   switch (static_cast<Alu>(a))
   {
   case Alu::Set:
      return value;
   case Alu::Add:
      return old + value;
   case Alu::Sub:
      return old - value;
   case Alu::Or:
      return old | value;
   case Alu::Bic:
      return old & ~value;
   case Alu::NUM_ALUS:
      break;
   }
   return old;
}

/// Reads a little-endian variable of 1, 2 or 4 bytes.
/// @param p      its bytes
/// @param width  1, 2 or 4
/// @return its value (zero-extended)
constexpr uint32_t load(const uint8_t* p, uint8_t width)
{
   uint32_t v = 0;
   // the highest byte first, each shifted down a byte by the next
   for (uint8_t i = width; i > 0; --i)
   {
      v = (v << kByteBits) | p[i - 1];
   }
   return v;
}

/// Writes a little-endian variable of 1, 2 or 4 bytes (the value's low bytes).
/// @param p      its bytes
/// @param width  1, 2 or 4
/// @param value  the value
constexpr void store(uint8_t* p, uint8_t width, uint32_t value)
{
   for (uint8_t i = 0; i < width; ++i)
   {
      p[i] = static_cast<uint8_t>(value >> (i * kByteBits));
   }
}

/// Bytes an operand of a type takes in the code.
/// @param t  the type
/// @return 1, 2 or 4
constexpr uint8_t type_bytes(Type t)
{
   switch (t)
   {
   case Type::U8:
   case Type::S8:
      return kByte;
   case Type::U16:
   case Type::S16:
      return kHalf;
   case Type::U32:
   case Type::Code:
   case Type::Ref:
   case Type::NUM_TYPES:
      break;
   }
   return kWord;
}

/// Decodes an operand's bytes by its type (sign-extending the signed ones).
/// @param p  the operand's bytes
/// @param t  its type
/// @return the value
constexpr uint32_t decode(const uint8_t* p, Type t)
{
   const uint32_t raw = load(p, type_bytes(t));
   switch (t)
   {
   case Type::S8:
      return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
   case Type::S16:
      return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
   default:
      break;
   }
   return raw;
}

}   // namespace capsule::vm_ext
