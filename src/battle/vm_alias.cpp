/// @file
/// The battle script machine's aliases (capsule/battle/vm.h run_alias(), D-39): a game opcode run as a fixed
/// sequence of the generic steps of capsule/core/vm_ext.h, its operand layout given as data.
#include "capsule/battle/vm.h"

namespace capsule::battle::vm
{
namespace
{

namespace ext = vm_ext;

/// Faults the machine.
/// @param m    the machine
/// @param why  the fault
void fail(Machine& m, Fault why)
{
   m.status = Status::Fault;
   m.fault = why;
}

/// Whether the machine still runs (a step may fault it, end it or stop it).
/// @param m  the machine
/// @return true while Running
bool running(const Machine& m)
{
   return m.status == Status::Running;
}

/// What one alias step works with.
struct Run
{
   Machine& m;              ///< The machine.
   const AliasSet& set;     ///< The game's steps and resolver.
   void* context;           ///< The game's.
   uint32_t size;           ///< Bytes of the command.
};

/// An argument's value: an operand read at its offset by its type (a Code operand: its reference's payload), or
/// its constant.
/// @param r  the run
/// @param a  the argument
/// @return the value (0 for an unused argument)
uint32_t value_of(Run& r, const ext::Arg& a)
{
   if (a.from == ext::From::Constant)
   {
      return a.type == ext::Type::Code ? ref_payload(a.value) : a.value;
   }
   if (a.from != ext::From::Operand)
   {
      return 0;
   }
   const uint8_t bytes = ext::type_bytes(a.type);
   const uint8_t* p = code_bytes(r.m, r.m.ip + a.at, bytes);
   if (p == nullptr)
   {
      return 0;
   }
   const uint32_t v = ext::decode(p, a.type);
   return a.type == ext::Type::Code ? ref_payload(v) : v;
}

/// The bytes a reference argument names: a Code reference inside the code, any other through the game's
/// resolver.
/// @param r      the run
/// @param a      the argument (a Ref operand or constant)
/// @param bytes  bytes read or written there
/// @param write  the step writes them (a Code reference is never written)
/// @return them, or nullptr (the machine faulted)
uint8_t* memory_of(Run& r, const ext::Arg& a, uint16_t bytes, bool write)
{
   const Ref ref = value_of(r, a);
   if (!running(r.m))
   {
      return nullptr;
   }
   uint8_t* p = r.set.resolve != nullptr ? r.set.resolve(r.m, ref, bytes, r.context) : nullptr;
   if (p == nullptr && ref_kind(ref) == RefKind::Code && !write)
   {
      // data inside the code (a list a script carries): read only
      p = const_cast<uint8_t*>(code_bytes(r.m, ref_payload(ref), bytes));
   }
   if (p == nullptr && running(r.m))
   {
      fail(r.m, Fault::BadRef);
   }
   return p;
}

/// Goes to a code offset (a target argument).
/// @param r  the run
/// @param a  the argument
/// @return Flow::Done
ext::Flow go(Run& r, const ext::Arg& a)
{
   const uint32_t to = value_of(r, a);
   if (running(r.m))
   {
      jump(r.m, to);
   }
   return ext::Flow::Done;
}

/// One step.
/// @param r  the run
/// @param s  the step
/// @return what it did
ext::Flow run_step(Run& r, const ext::Step& s)
{
   Machine& m = r.m;
   switch (s.op)
   {
   case ext::Op::Nop:
      return ext::Flow::Next;
   case ext::Op::Jump:
      return go(r, s.args[ext::kArg0]);
   case ext::Op::JumpIf:
   {
      const uint8_t c = static_cast<uint8_t>(value_of(r, s.args[ext::kArg0]));
      const uint8_t* p = memory_of(r, s.args[ext::kArg1], s.width, false);
      const uint32_t with = value_of(r, s.args[ext::kArg2]);
      if (p == nullptr || !running(m))
      {
         return ext::Flow::Done;
      }
      return ext::compare(c, ext::load(p, s.width), with) ? go(r, s.args[ext::kArg3]) : ext::Flow::Next;
   }
   case ext::Op::JumpIfBytes:
   {
      const uint32_t n = value_of(r, s.args[ext::kArg2]);
      const uint8_t* a = memory_of(r, s.args[ext::kArg0], static_cast<uint16_t>(n), false);
      const uint8_t* b = memory_of(r, s.args[ext::kArg1], static_cast<uint16_t>(n), false);
      if (a == nullptr || b == nullptr || !running(m))
      {
         return ext::Flow::Done;
      }
      bool same = true;
      for (uint32_t i = 0; i < n && same; ++i)
      {
         same = a[i] == b[i];
      }
      const bool want_same = static_cast<ext::Compare>(s.mode) == ext::Compare::Equal;
      return same == want_same ? go(r, s.args[ext::kArg3]) : ext::Flow::Next;
   }
   case ext::Op::Call:
   {
      const uint32_t to = value_of(r, s.args[ext::kArg0]);
      if (running(m))
      {
         call(m, to, m.ip + r.size);
      }
      return ext::Flow::Done;
   }
   case ext::Op::Return:
      ret(m);
      return ext::Flow::Done;
   case ext::Op::End:
      end(m);
      return ext::Flow::Done;
   case ext::Op::Yield:
      yield(m);
      return ext::Flow::Next;
   case ext::Op::Pause:
   {
      const uint16_t frames = static_cast<uint16_t>(value_of(r, s.args[ext::kArg0]));
      return running(m) && pause(m, frames) ? ext::Flow::Next : ext::Flow::Done;
   }
   case ext::Op::PauseIfSet:
   {
      uint8_t* flag = memory_of(r, s.args[ext::kArg0], ext::kByte, true);
      const uint16_t frames = static_cast<uint16_t>(value_of(r, s.args[ext::kArg1]));
      if (flag == nullptr || !running(m))
      {
         return ext::Flow::Done;
      }
      if (*flag == 0)
      {
         return ext::Flow::Next;
      }
      // the flag set: the frames counted, then the flag cleared on the way out
      if (!pause(m, frames))
      {
         return ext::Flow::Done;
      }
      *flag = 0;
      return ext::Flow::Next;
   }
   case ext::Op::WaitUntil:
   {
      const uint8_t c = static_cast<uint8_t>(value_of(r, s.args[ext::kArg0]));
      const uint8_t* p = memory_of(r, s.args[ext::kArg1], s.width, false);
      const uint32_t with = value_of(r, s.args[ext::kArg2]);
      if (p == nullptr || !running(m))
      {
         return ext::Flow::Done;
      }
      if (ext::compare(c, ext::load(p, s.width), with))
      {
         return ext::Flow::Next;
      }
      yield(m);
      return ext::Flow::Done;
   }
   case ext::Op::Hold:
      return ext::Flow::Done;
   case ext::Op::Modify:
   {
      uint8_t* p = memory_of(r, s.args[ext::kArg0], s.width, true);
      const uint32_t v = value_of(r, s.args[ext::kArg1]);
      if (p != nullptr && running(m))
      {
         ext::store(p, s.width, ext::apply(s.mode, ext::load(p, s.width), v));
      }
      return running(m) ? ext::Flow::Next : ext::Flow::Done;
   }
   case ext::Op::Copy:
   {
      const uint32_t n = value_of(r, s.args[ext::kArg2]);
      uint8_t* dest = memory_of(r, s.args[ext::kArg0], static_cast<uint16_t>(n), true);
      const uint8_t* src = memory_of(r, s.args[ext::kArg1], static_cast<uint16_t>(n), false);
      for (uint32_t i = 0; dest != nullptr && src != nullptr && running(m) && i < n; ++i)
      {
         dest[i] = src[i];
      }
      return running(m) ? ext::Flow::Next : ext::Flow::Done;
   }
   case ext::Op::NUM_OPS:
      break;
   }
   fail(m, Fault::BadStep);
   return ext::Flow::Done;
}

}   // namespace

void run_alias(Machine& m, const vm_ext::Alias& alias, const AliasSet& set, void* context)
{
   // the whole command first: an operand past the code faults before any step runs
   if (code_bytes(m, m.ip, alias.size) == nullptr)
   {
      return;
   }
   if (static_cast<uint32_t>(alias.first) + alias.count > set.step_count)
   {
      fail(m, Fault::BadStep);
      return;
   }
   Run r{m, set, context, alias.size};
   for (uint16_t i = alias.first; i < alias.first + alias.count; ++i)
   {
      if (run_step(r, set.steps[i]) == ext::Flow::Done)
      {
         return;
      }
      if (!running(m))
      {
         return;
      }
   }
   advance(m, alias.size);
}

}   // namespace capsule::battle::vm
