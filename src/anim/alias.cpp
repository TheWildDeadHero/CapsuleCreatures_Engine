/// @file
/// The animation-script player's aliases (capsule/anim/player.h run_alias(), D-39): a game opcode run as a fixed
/// sequence of the generic steps of capsule/core/vm_ext.h, its operand layout given as data.
#include "capsule/anim/player.h"

namespace capsule::anim
{
namespace
{

namespace ext = vm_ext;

/// What one alias step works with.
struct Run
{
   Player& p;              ///< The player.
   const AliasSet& set;    ///< The game's steps and resolver.
   uint32_t at;            ///< The command's code address.
   uint32_t size;          ///< Its bytes.
};

/// An argument's value: an operand read at its offset by its type, or its constant.
/// @param r  the run
/// @param a  the argument
/// @return the value (0 for an unused argument)
uint32_t value_of(const Run& r, const ext::Arg& a)
{
   if (a.from == ext::From::Constant)
   {
      return a.value;
   }
   if (a.from != ext::From::Operand)
   {
      return 0;
   }
   // run_alias() checked that the whole command lies inside the code
   return ext::decode(r.p.bank->code + r.at + a.at, a.type);
}

/// The bytes a reference argument names, through the game's resolver.
/// @param r      the run
/// @param a      the argument
/// @param bytes  bytes read or written there
/// @return them, or nullptr (the player stopped with BadOperand)
uint8_t* memory_of(Run& r, const ext::Arg& a, uint16_t bytes)
{
   uint8_t* m = r.set.resolve != nullptr ? r.set.resolve(r.p, value_of(r, a), bytes) : nullptr;
   if (m == nullptr)
   {
      fail(r.p, PlayerError::BadOperand);
   }
   return m;
}

/// Goes to a code address (a target argument; checked by run_alias()).
/// @param r  the run
/// @param a  the argument
/// @return Flow::Done
ext::Flow go(Run& r, const ext::Arg& a)
{
   jump(r.p, value_of(r, a));
   return ext::Flow::Done;
}

/// One step.
/// @param r  the run
/// @param s  the step
/// @return what it did
ext::Flow run_step(Run& r, const ext::Step& s)
{
   switch (s.op)
   {
   case ext::Op::Nop:
      return ext::Flow::Next;
   case ext::Op::Jump:
      return go(r, s.args[ext::kArg0]);
   case ext::Op::JumpIf:
   case ext::Op::WaitUntil:
   {
      const uint8_t c = static_cast<uint8_t>(value_of(r, s.args[ext::kArg0]));
      const uint8_t* m = memory_of(r, s.args[ext::kArg1], s.width);
      if (m == nullptr)
      {
         return ext::Flow::Done;
      }
      const bool holds = ext::compare(c, ext::load(m, s.width), value_of(r, s.args[ext::kArg2]));
      if (s.op == ext::Op::JumpIf)
      {
         return holds ? go(r, s.args[ext::kArg3]) : ext::Flow::Next;
      }
      if (holds)
      {
         return ext::Flow::Next;
      }
      // not yet: the same command runs again next frame
      yield(r.p);
      return ext::Flow::Done;
   }
   case ext::Op::JumpIfBytes:
   {
      const uint16_t n = static_cast<uint16_t>(value_of(r, s.args[ext::kArg2]));
      const uint8_t* a = memory_of(r, s.args[ext::kArg0], n);
      const uint8_t* b = a != nullptr ? memory_of(r, s.args[ext::kArg1], n) : nullptr;
      if (b == nullptr)
      {
         return ext::Flow::Done;
      }
      bool same = true;
      for (uint16_t i = 0; i < n && same; ++i)
      {
         same = a[i] == b[i];
      }
      const bool want_same = static_cast<ext::Compare>(s.mode) == ext::Compare::Equal;
      return same == want_same ? go(r, s.args[ext::kArg3]) : ext::Flow::Next;
   }
   case ext::Op::Yield:
      yield(r.p);
      return ext::Flow::Next;
   case ext::Op::Hold:
      // stays on the command without waiting: the frame's loop runs it again (a command that never advances)
      return ext::Flow::Done;
   case ext::Op::Modify:
   {
      uint8_t* m = memory_of(r, s.args[ext::kArg0], s.width);
      if (m == nullptr)
      {
         return ext::Flow::Done;
      }
      ext::store(m, s.width, ext::apply(s.mode, ext::load(m, s.width), value_of(r, s.args[ext::kArg1])));
      return ext::Flow::Next;
   }
   case ext::Op::Copy:
   {
      const uint16_t n = static_cast<uint16_t>(value_of(r, s.args[ext::kArg2]));
      uint8_t* dest = memory_of(r, s.args[ext::kArg0], n);
      const uint8_t* src = dest != nullptr ? memory_of(r, s.args[ext::kArg1], n) : nullptr;
      if (src == nullptr)
      {
         return ext::Flow::Done;
      }
      for (uint16_t i = 0; i < n; ++i)
      {
         dest[i] = src[i];
      }
      return ext::Flow::Next;
   }
   default:
      break;
   }
   fail(r.p, PlayerError::BadOpcode);
   return ext::Flow::Done;
}

constexpr uint32_t kNoBadTarget = 0xFFFFFFFFu;   ///< bad_target(): every target inside the code.

/// The first code-address operand (in operand order) of an alias's steps that lies outside the code: the
/// operand readers' check, made before any step runs, so an alias stops where its native would.
/// @param r      the run
/// @param alias  the alias
/// @return the offset just past that operand (where the native's reader leaves the program counter), or
///         kNoBadTarget
uint32_t bad_target(const Run& r, const ext::Alias& alias)
{
   uint32_t first = kNoBadTarget;
   for (uint16_t i = alias.first; i < alias.first + alias.count; ++i)
   {
      for (const ext::Arg& a : r.set.steps[i].args)
      {
         const bool code = a.from == ext::From::Operand && a.type == ext::Type::Code;
         if (code && value_of(r, a) >= r.p.bank->code_size && a.at < first)
         {
            first = a.at;
         }
      }
   }
   return first == kNoBadTarget ? first : first + ext::type_bytes(ext::Type::Code);
}

}   // namespace

void run_alias(Player& p, const vm_ext::Alias& alias, const AliasSet& set)
{
   const uint32_t at = p.pc;
   const uint32_t code = p.bank->code_size;
   if (at > code || alias.size > code - at || static_cast<uint32_t>(alias.first) + alias.count > set.step_count)
   {
      fail(p, at > code || alias.size > code - at ? PlayerError::BadOperand : PlayerError::BadOpcode);
      return;
   }
   Run r{p, set, at, alias.size};
   const uint32_t bad = bad_target(r, alias);
   if (bad != kNoBadTarget)
   {
      p.pc = at + bad;
      fail(p, PlayerError::BadOperand);
      return;
   }
   for (uint16_t i = alias.first; i < alias.first + alias.count; ++i)
   {
      if (run_step(r, set.steps[i]) == ext::Flow::Done || !p.active)
      {
         return;
      }
   }
   p.pc = at + alias.size;
}

}   // namespace capsule::anim
