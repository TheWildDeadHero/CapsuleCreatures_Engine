/// @file
/// Effect VM interpreter (see capsule/battle/effect.h).
#include "capsule/battle/effect.h"

#include "capsule/core/assert.h"
#include "capsule/core/endian.h"

namespace capsule::battle::effect
{
namespace
{

/// @name Immediate offsets (bytes after the opcode)
///@{
constexpr int kImmFirst = 0;    ///< First immediate byte.
constexpr int kImmSecond = 1;   ///< Second immediate byte.
constexpr int kImmThird = 2;    ///< Third immediate byte.
constexpr int kForEachOffset = 3;   ///< ForEach: where its off16 starts (after set8, subj8, local8).
constexpr int kLoopOffset = 1;  ///< Loop / Next: where their off16 starts (after local8).
///@}

/// @name Stack slots below `top` (one past the top value)
///@{
constexpr int kBelowTop = 2;   ///< The value under the top.
constexpr int kThirdTop = 3;   ///< The third value from the top.
constexpr int kFourthTop = 4;  ///< The fourth value from the top.
///@}

/// Bits of a battler mask that name battlers.
constexpr uint32_t kBattlerMask = kMaxBattlers >= kMaskBits ? ~0u : (1u << kMaxBattlers) - 1u;
/// Highest fault count (Machine::faults saturates).
constexpr uint16_t kMaxFaults = 0xFFFF;
/// Battler subjects (holder, other, user, target) are the codes below this.
constexpr uint8_t kBattlerSubjects = 4;
/// ForEach keeps its remaining mask in the local after the loop variable.
constexpr int kMaskLocal = 1;

/// Records a fault.
/// @param m  bookkeeping
/// @param f  what went wrong
void count_fault(Machine& m, Fault f)
{
   if (m.faults < kMaxFaults)
   {
      ++m.faults;
   }
   m.last_fault = f;
}

/// One execution of a run: the run, its code and where to report.
struct Exec
{
   Machine& m;             ///< Bookkeeping.
   const Bank& bank;       ///< The code.
   EffectRun& r;           ///< The run.
   HostState& host;        ///< The embedder.
   const uint8_t* code;    ///< The running program's first byte.
   uint16_t length;        ///< Its length.
};

/// Points an execution at the run's current program.
/// @param x  the execution
/// @return false when the program index is out of range
bool load_program(Exec& x)
{
   if (x.r.program >= x.bank.program_count)
   {
      return false;
   }
   const ProgramInfo& p = x.bank.programs[x.r.program];
   if (p.offset + p.length > x.bank.code_size)
   {
      return false;
   }
   x.code = x.bank.code + p.offset;
   x.length = p.length;
   return true;
}

/// Ends a run with a fault.
/// @param x  the execution
/// @param f  what went wrong
/// @return Status::Fault
Status fail_with(Exec& x, Fault f)
{
   count_fault(x.m, f);
   x.r.state = RunState::Idle;
   return Status::Fault;
}

/// Resolves a subject code.
/// @param r     the run (context registers, locals)
/// @param subj  the code
/// @param out   the place
/// @return false for a code that does not exist or a local without a battler in it
bool resolve(const EffectRun& r, uint8_t subj, Place& out)
{
   const uint8_t base = subj & kSubjBaseMask;
   const bool side = (subj & kSubjSide) != 0;
   out.move = r.ctx.move;
   out.battler = r.ctx.holder;
   int32_t battler = -1;
   if (base < kBattlerSubjects)
   {
      // The battler registers, in subject code order.
      const uint8_t regs[kBattlerSubjects] = {r.ctx.holder, r.ctx.other, r.ctx.user, r.ctx.target};
      battler = regs[base];
   }
   else if (base >= kSubjLocal && base < kSubjLocal + kEffectLocals)
   {
      battler = r.locals[base - kSubjLocal];
      if (battler < 0 || battler >= kMaxBattlers)
      {
         return false;
      }
   }
   else if (base >= kSubjBattler && base < kSubjBattler + kMaxBattlers)
   {
      battler = base - kSubjBattler;   // B1: a fixed battler (a combination's input)
   }
   if (battler >= 0)
   {
      out.battler = static_cast<uint8_t>(battler);
      out.kind = side ? PlaceKind::Side : PlaceKind::Battler;
      return true;
   }
   if (side)
   {
      return false;   // only battlers have sides
   }
   switch (base)
   {
   case kSubjMove:
      out.kind = PlaceKind::Move;
      return true;
   case kSubjField:
      out.kind = PlaceKind::Field;
      return true;
   case kSubjNone:
      out.kind = PlaceKind::None;
      return true;
   default:
      return false;
   }
}

/// Lowest set bit's index.
/// @param mask  non-zero mask
/// @return its index
int32_t lowest_bit(uint32_t mask)
{
   return __builtin_ctz(mask);
}

/// A comparison or arithmetic op on two operands.
/// @param op  the opcode (Add ... Ge)
/// @param a   left
/// @param b   right
/// @return the result
int32_t binary(Op op, int32_t a, int32_t b)
{
   switch (op)
   {
   case Op::Add:
      return sat_add(a, b);
   case Op::Sub:
      return sat_sub(a, b);
   case Op::Mul:
      return sat_mul(a, b);
   case Op::Div:
      return safe_div(a, b);
   case Op::Mod:
      return safe_mod(a, b);
   case Op::Min:
      return min_value(a, b);
   case Op::Max:
      return max_value(a, b);
   case Op::MulQ8:
      return mul_q8(a, b);
   case Op::Shl:
      return sat_shl(a, b);
   case Op::Shr:
      return arith_shr(a, b);
   case Op::Eq:
      return a == b;
   case Op::Ne:
      return a != b;
   case Op::Lt:
      return a < b;
   case Op::Le:
      return a <= b;
   case Op::Gt:
      return a > b;
   default:
      return a >= b;
   }
}

/// Whether an op is a primitive the host performs.
/// @param op  the opcode
/// @return true for Damage ... Raise except Modify (Odds is not one)
bool is_primitive(Op op)
{
   // Modify changes the context and Odds draws (E9, appended after the primitives): the VM runs both itself
   return op >= Op::Damage && op != Op::Modify && op != Op::Odds;
}

/// Decodes a primitive's immediates and operands.
/// @param x     the execution
/// @param op    the opcode
/// @param imm   its immediates
/// @param argc  operands it pops
/// @param a     filled in
/// @return false for a subject code that does not resolve
bool decode_action(Exec& x, Op op, const uint8_t* imm, int argc, Action& a)
{
   a = Action{};
   a.op = op;
   a.argc = static_cast<uint8_t>(argc);
   for (int i = 0; i < argc; ++i)
   {
      a.args[i] = x.r.stack[x.r.sp - argc + i];
   }
   a.at = Place{PlaceKind::None, x.r.ctx.holder, x.r.ctx.move};
   a.about = a.at;
   switch (op_info(op).imm)
   {
   case Imm::Subj:
      return resolve(x.r, imm[kImmFirst], a.at);
   case Imm::SubjId:
      a.id = imm[kImmSecond];
      return resolve(x.r, imm[kImmFirst], a.at);
   case Imm::SubjIdFlags:
      a.id = imm[kImmSecond];
      a.flags = imm[kImmThird];
      return resolve(x.r, imm[kImmFirst], a.at);
   case Imm::SubjFlags:
   case Imm::SubjCount:
      a.flags = imm[kImmSecond];
      return resolve(x.r, imm[kImmFirst], a.at);
   case Imm::Id:
      a.id = imm[kImmFirst];
      return true;
   case Imm::Text:
      a.id = load_le16(imm);
      return true;
   case Imm::IdSubj:
      a.id = imm[kImmFirst];
      return resolve(x.r, imm[kImmSecond], a.at);
   case Imm::IdSubjSubj:
      a.id = imm[kImmFirst];
      return resolve(x.r, imm[kImmSecond], a.at) && resolve(x.r, imm[kImmThird], a.about);
   case Imm::IdFlags:
      a.id = imm[kImmFirst];
      a.flags = imm[kImmSecond];
      return true;
   default:
      return true;
   }
}

/// Runs a primitive (the stack has been checked for its operands and result).
/// @param x     the execution
/// @param op    the opcode
/// @param imm   its immediates
/// @param next  offset of the next instruction
/// @param argc  operands it pops
/// @param done  set to how the run ends, when it does
/// @return true to go on, false when *done is set
bool primitive(Exec& x, Op op, const uint8_t* imm, uint16_t next, int argc, Status& done)
{
   EffectRun& r = x.r;
   if (op == Op::Ask && r.resumable == 0)
   {
      done = fail_with(x, Fault::Suspend);
      return false;
   }
   Action a{};
   if (!decode_action(x, op, imm, argc, a))
   {
      done = fail_with(x, Fault::BadSubject);
      return false;
   }
   r.sp = static_cast<uint8_t>(r.sp - argc);
   r.pc = next;   // a nested run or a suspension sees the run past this instruction
   const Outcome o = host_action(x.host, r, a);
   if (o.stop || o.fail)
   {
      // the host ended the run (an immune target ends a hit, a condition's try_start failed the move, ...)
      r.state = RunState::Idle;
      done = o.fail ? Status::Failed : Status::Stopped;
      return false;
   }
   switch (op)
   {
   case Op::Immune:
      r.state = RunState::Idle;
      done = Status::Immune;
      return false;
   case Op::Block:
      r.state = RunState::Idle;
      done = Status::Blocked;
      return false;
   case Op::Ask:
      if (o.suspend)
      {
         r.state = RunState::Asking;
         done = Status::Asked;
         return false;
      }
      break;
   case Op::Damage:
      r.ctx.dealt = sat_add(r.ctx.dealt, o.value);   // the hit's damage counts towards `dealt`
      break;
   default:
      break;
   }
   if (op_info(op).pushes != 0)
   {
      r.stack[r.sp++] = o.value;
   }
   return true;
}

/// Executes the run until it ends, yields or asks.
/// @param x  the execution
/// @return how it stopped
Status execute(Exec& x)
{
   EffectRun& r = x.r;
   if (!load_program(x))
   {
      return fail_with(x, Fault::BadProgram);
   }
   int ops = 0;
   while (true)
   {
      // Backstop: the compiler bounds every program; only broken code gets here.
      if (++ops > kEffectOpsPerRun)
      {
         ENGINE_ASSERT(ops <= kEffectOpsPerRun);
         return fail_with(x, Fault::OpBudget);
      }
      if (r.pc >= x.length)
      {
         return fail_with(x, Fault::BadProgram);
      }
      const uint8_t raw = x.code[r.pc];
      if (raw >= kOpCount)
      {
         return fail_with(x, Fault::BadOpcode);
      }
      const Op op = static_cast<Op>(raw);
      const OpInfo& info = op_info(op);
      if (r.pc + 1 + info.size > x.length)
      {
         return fail_with(x, Fault::BadProgram);
      }
      const uint8_t* imm = x.code + r.pc + 1;
      const uint16_t next = static_cast<uint16_t>(r.pc + 1 + info.size);

      // Stack effect, checked once for every op (Pick and SetTypes pop n more).
      int pops = info.pops;
      if (op == Op::Pick || op == Op::SetTypes)
      {
         pops += imm[op == Op::Pick ? kImmFirst : kImmSecond];
      }
      if (op == Op::SetTypes && pops > kMaxActionArgs)
      {
         return fail_with(x, Fault::BadOperand);
      }
      if (r.sp < pops)
      {
         return fail_with(x, Fault::StackUnderflow);
      }
      if (r.sp - pops + info.pushes > kEffectStack)
      {
         return fail_with(x, Fault::StackOverflow);
      }
      int32_t* top = r.stack + r.sp;   // one past the top: top[-1] is the top value

      if (is_primitive(op))
      {
         Status done = Status::Done;
         if (!primitive(x, op, imm, next, pops, done))
         {
            return done;
         }
         continue;
      }

      uint16_t pc = next;
      switch (op)
      {
      case Op::End:
         r.state = RunState::Idle;
         return Status::Done;
      case Op::Push8:
         top[0] = static_cast<int8_t>(imm[kImmFirst]);
         ++r.sp;
         break;
      case Op::Push16:
         top[0] = load_le16s(imm);
         ++r.sp;
         break;
      case Op::Push32:
         top[0] = load_le32s(imm);
         ++r.sp;
         break;
      case Op::Dup:
         top[0] = top[-1];
         ++r.sp;
         break;
      case Op::Drop:
         --r.sp;
         break;
      case Op::Swap:
      {
         const int32_t t = top[-1];
         top[-1] = top[-kBelowTop];
         top[-kBelowTop] = t;
         break;
      }
      case Op::Load:
      case Op::Store:
      {
         const uint8_t l = imm[kImmFirst];
         if (l >= kEffectLocals)
         {
            return fail_with(x, Fault::BadLocal);
         }
         if (op == Op::Load)
         {
            top[0] = r.locals[l];
            ++r.sp;
         }
         else
         {
            r.locals[l] = top[-1];
            --r.sp;
         }
         break;
      }
      case Op::Neg:
         top[-1] = sat_neg(top[-1]);
         break;
      case Op::Abs:
         top[-1] = sat_abs(top[-1]);
         break;
      case Op::Not:
         top[-1] = top[-1] == 0;
         break;
      case Op::Clamp:
         top[-kThirdTop] = clamp_value(top[-kThirdTop], top[-kBelowTop], top[-1]);
         r.sp = static_cast<uint8_t>(r.sp - kPop2);
         break;
      case Op::Pick:
      {
         // Stack: index, then n values; the index is clamped into the table.
         const int n = imm[kImmFirst];
         if (n == 0)
         {
            return fail_with(x, Fault::BadOperand);
         }
         const int32_t i = clamp_value(top[-n - 1], 0, n - 1);
         top[-n - 1] = top[-n + i];
         r.sp = static_cast<uint8_t>(r.sp - n);
         break;
      }
      case Op::Jmp:
      case Op::Jz:
      case Op::Jnz:
      {
         const uint16_t target = load_le16(imm);
         if (target < next || target >= x.length)
         {
            return fail_with(x, Fault::BadJump);
         }
         bool jump = true;
         if (op != Op::Jmp)
         {
            const int32_t c = top[-1];
            --r.sp;
            jump = op == Op::Jz ? c == 0 : c != 0;
         }
         if (jump)
         {
            pc = target;
         }
         break;
      }
      case Op::Loop:
      {
         // --counter; back to the body while it is above 0.
         const uint8_t l = imm[kImmFirst];
         const uint16_t target = load_le16(imm + kLoopOffset);
         if (l >= kEffectLocals)
         {
            return fail_with(x, Fault::BadLocal);
         }
         if (target > r.pc)
         {
            return fail_with(x, Fault::BadJump);
         }
         r.locals[l] = sat_sub(r.locals[l], 1);
         if (r.locals[l] > 0)
         {
            pc = target;
         }
         break;
      }
      case Op::ForEach:
      case Op::Next:
      {
         const bool first = op == Op::ForEach;
         const uint8_t l = imm[first ? kImmThird : kImmFirst];
         const uint16_t target = load_le16(imm + (first ? kForEachOffset : kLoopOffset));
         if (l + kMaskLocal >= kEffectLocals)
         {
            return fail_with(x, Fault::BadLocal);
         }
         if (first ? (target < next || target >= x.length) : target > r.pc)
         {
            return fail_with(x, Fault::BadJump);
         }
         uint32_t mask = 0;
         if (first)
         {
            const uint8_t set = imm[kImmFirst];
            Place at{};
            if (set >= static_cast<uint8_t>(BattlerSet::NUM_SETS))
            {
               return fail_with(x, Fault::BadOperand);
            }
            if (!resolve(r, imm[kImmSecond], at))
            {
               return fail_with(x, Fault::BadSubject);
            }
            mask = host_battlers(x.host, r, static_cast<BattlerSet>(set), at);
         }
         else
         {
            mask = static_cast<uint32_t>(r.locals[l + kMaskLocal]);
         }
         mask &= kBattlerMask;
         if (mask == 0)
         {
            // ForEach: an empty set skips the loop; Next: the last round is done.
            if (first)
            {
               pc = target;
            }
            break;
         }
         r.locals[l] = lowest_bit(mask);
         r.locals[l + kMaskLocal] = static_cast<int32_t>(mask & (mask - 1u));
         if (!first)
         {
            pc = target;
         }
         break;
      }
      case Op::Call:
      {
         const uint16_t callee = load_le16(imm);
         if (r.call_depth >= kEffectCallDepth)
         {
            return fail_with(x, Fault::CallDepth);
         }
         r.calls[r.call_depth++] = CallFrame{r.program, next};
         r.program = callee;
         if (!load_program(x))
         {
            return fail_with(x, Fault::BadProgram);
         }
         pc = 0;
         break;
      }
      case Op::Ret:
         if (r.call_depth == 0)
         {
            r.state = RunState::Idle;
            return Status::Done;
         }
         --r.call_depth;
         r.program = r.calls[r.call_depth].program;
         if (!load_program(x))
         {
            return fail_with(x, Fault::BadProgram);
         }
         pc = r.calls[r.call_depth].pc;
         break;
      case Op::Yield:
         if (r.resumable == 0)
         {
            return fail_with(x, Fault::Suspend);
         }
         r.pc = next;
         return Status::Yielded;
      case Op::Stop:
         r.state = RunState::Idle;
         return Status::Stopped;
      case Op::Fail:
         r.state = RunState::Idle;
         return Status::Failed;
      case Op::Get:
      case Op::Set:
      {
         const uint8_t ref = imm[kImmFirst];
         Place at{};
         if (ref >= static_cast<uint8_t>(Ref::NUM_REFS))
         {
            return fail_with(x, Fault::BadOperand);
         }
         if (!resolve(r, imm[kImmSecond], at))
         {
            return fail_with(x, Fault::BadSubject);
         }
         const Ref what = static_cast<Ref>(ref);
         if (op == Op::Set)
         {
            // the subject and the context registers are the VM's own: not writable
            if (vm_ref(what))
            {
               return fail_with(x, Fault::BadOperand);
            }
            host_set(x.host, r, what, at, imm[kImmThird], top[-1]);
            --r.sp;
            break;
         }
         int32_t v = 0;
         switch (what)
         {
         case Ref::Battler:
            if (at.kind != PlaceKind::Battler)
            {
               return fail_with(x, Fault::BadSubject);
            }
            v = at.battler;
            break;
         case Ref::Value:
            v = r.ctx.value;
            break;
         case Ref::Dealt:
            v = r.ctx.dealt;
            break;
         case Ref::Move:
            v = r.ctx.move;
            break;
         case Ref::Owner:
            v = r.ctx.owner;
            break;
         default:
            v = host_get(x.host, r, what, at, imm[kImmThird]);
            break;
         }
         top[0] = v;
         ++r.sp;
         break;
      }
      case Op::Has:
      {
         const uint8_t kind = imm[kImmFirst];
         Place at{};
         if (kind >= static_cast<uint8_t>(HasKind::NUM_KINDS))
         {
            return fail_with(x, Fault::BadOperand);
         }
         if (!resolve(r, imm[kImmSecond], at))
         {
            return fail_with(x, Fault::BadSubject);
         }
         top[0] = host_has(x.host, r, static_cast<HasKind>(kind), at, imm[kImmThird]) ? 1 : 0;
         ++r.sp;
         break;
      }
      case Op::Subj:
      {
         Place at{};
         if (!resolve(r, imm[kImmFirst], at) || at.kind != PlaceKind::Battler)
         {
            return fail_with(x, Fault::BadSubject);
         }
         top[0] = at.battler;
         ++r.sp;
         break;
      }
      case Op::RoleGet:
      case Op::RoleSet:
      {
         Place at{};
         if (!resolve(r, imm[kImmFirst], at) || at.kind != PlaceKind::Battler)
         {
            return fail_with(x, Fault::BadSubject);
         }
         if (op == Op::RoleGet)
         {
            top[0] = host_role_get(x.host, r, at.battler, imm[kImmSecond], imm[kImmThird]);
            ++r.sp;
         }
         else
         {
            host_role_set(x.host, r, at.battler, imm[kImmSecond], imm[kImmThird], top[-1]);
            --r.sp;
         }
         break;
      }
      case Op::Rand:
      {
         // n <= 0 draws nothing and gives 0 (like random below(0)).
         const int32_t n = top[-1];
         top[-1] = n <= 0 ? 0 : static_cast<int32_t>(host_below(x.host, static_cast<uint32_t>(n)));
         break;
      }
      case Op::Between:
      {
         // hi < lo draws nothing and gives lo.
         const int32_t lo = top[-kBelowTop];
         const int32_t hi = top[-1];
         top[-kBelowTop] = hi < lo ? lo : host_between(x.host, lo, hi);
         --r.sp;
         break;
      }
      case Op::Chance:
      {
         // m <= 0 or n <= 0 draws nothing and fails.
         const int32_t n = top[-kBelowTop];
         const int32_t m = top[-1];
         const bool ok = m > 0 && n > 0 && host_chance(x.host, static_cast<uint32_t>(n), static_cast<uint32_t>(m));
         top[-kBelowTop] = ok ? 1 : 0;
         --r.sp;
         break;
      }
      case Op::Odds:
      {
         // n m by on: like Chance (m <= 0 or n <= 0 draws nothing and fails), through the host's effect odds
         const int32_t n = top[-kFourthTop];
         const int32_t m = top[-kThirdTop];
         const int32_t by = top[-kBelowTop];
         const int32_t on = top[-1];
         if (by < 0 || by >= kMaxBattlers || on < 0 || on >= kMaxBattlers)
         {
            return fail_with(x, Fault::BadSubject);
         }
         const bool ok = m > 0 && n > 0 &&
                         host_odds(x.host, r, static_cast<uint32_t>(n), static_cast<uint32_t>(m),
                                   static_cast<uint8_t>(by), static_cast<uint8_t>(on));
         top[-kFourthTop] = ok ? 1 : 0;
         r.sp = static_cast<uint8_t>(r.sp - (kFourthTop - 1));
         break;
      }
      case Op::Modify:
      {
         const uint8_t mode = imm[kImmFirst];
         if (mode >= static_cast<uint8_t>(ModifyMode::NUM_MODES))
         {
            return fail_with(x, Fault::BadOperand);
         }
         r.ctx.value = modify_value(static_cast<ModifyMode>(mode), r.ctx.value, top[-1]);
         --r.sp;
         break;
      }
      default:
         // The two-operand arithmetic and comparisons.
         top[-kBelowTop] = binary(op, top[-kBelowTop], top[-1]);
         --r.sp;
         break;
      }
      r.pc = pc;
   }
}

/// One level of run nesting, held while a run executes: Machine::depth goes
/// up on construction and back down when the scope ends, on every path.
struct DepthScope
{
   Machine& machine;   ///< The machine whose depth this holds.

   /// Enters one level.
   /// @param m  bookkeeping
   explicit DepthScope(Machine& m) : machine(m)
   {
      ++machine.depth;
   }
   DepthScope(const DepthScope&) = delete;
   DepthScope& operator=(const DepthScope&) = delete;
   /// Leaves the level.
   ~DepthScope()
   {
      --machine.depth;
   }
};

/// Runs a run one level deeper.
/// @param m     bookkeeping
/// @param bank  the code
/// @param r     the run
/// @param host  the embedder
/// @return how it stopped
Status run_nested(Machine& m, const Bank& bank, EffectRun& r, HostState& host)
{
   if (m.depth >= kMaxEffectDepth)
   {
      count_fault(m, Fault::Depth);
      return Status::Skipped;
   }
   Exec x{m, bank, r, host, nullptr, 0};
   const DepthScope level(m);
   return execute(x);
}

} // namespace

void begin(EffectRun& r, uint16_t program, const Context& ctx, bool resumable)
{
   // Only the header and the locals: the operand stack is read below sp only
   // and the call frames below call_depth only, so their old contents never
   // show (clearing all 136 bytes per run was the cost of every hook).
   r.ctx = ctx;
   r.program = program;
   r.pc = 0;
   r.sp = 0;
   r.call_depth = 0;
   r.state = RunState::Running;
   r.resumable = resumable ? 1 : 0;
   for (int32_t& l : r.locals)
   {   // programs may read a local before writing it: it starts at 0
      l = 0;
   }
}

Status resume(Machine& m, const Bank& bank, EffectRun& r, HostState& host)
{
   switch (r.state)
   {
   case RunState::Running:
      return run_nested(m, bank, r, host);
   case RunState::Asking:
      return Status::Asked;   // still waits for answer()
   default:
      return Status::Done;
   }
}

Status answer(Machine& m, const Bank& bank, EffectRun& r, HostState& host, int32_t value)
{
   if (r.state != RunState::Asking || r.sp >= kEffectStack)
   {
      count_fault(m, Fault::Suspend);
      r.state = RunState::Idle;
      return Status::Fault;
   }
   r.stack[r.sp++] = value;
   r.state = RunState::Running;
   return run_nested(m, bank, r, host);
}

Status invoke(Machine& m, const Bank& bank, HostState& host, uint16_t program, Context& ctx)
{
   if (m.depth >= kMaxEffectDepth)
   {
      count_fault(m, Fault::Depth);
      return Status::Skipped;
   }
   EffectRun& frame = m.frames[m.depth];
   begin(frame, program, ctx, false);
   const Status s = run_nested(m, bank, frame, host);
   ctx = frame.ctx;
   return s;
}

} // namespace capsule::battle::effect
