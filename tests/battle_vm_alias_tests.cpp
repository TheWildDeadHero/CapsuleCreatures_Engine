/// @file
/// Tests for the battle script machine's aliases (capsule/battle/vm.h run_alias(), capsule/core/vm_ext.h, D-39).
/// The manifest tests/data/vm_ext/battle_alias_vm_ext.json declares one alias per generic step; tools/vm_ext.py
/// writes their dispatch (battle_alias_dispatch.inc, included below) and tools/script_bank.py assembles
/// tests/data/vm_ext/battle_alias_bank.json with the manifest's layouts. Parity: the same bank runs on a table of
/// hand-written natives of the same layouts (the way a game wrote them before D-39), and after every step both
/// machines and both contexts must be identical. Hand-made aliases test the faults.
#include "capsule/battle/vm.h"
#include "capsule/core/vm_ext.h"

#include "battle_alias_bank_ids.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace battle_alias_bank
{
extern const uint8_t kBank[];      ///< The assembled bank (generated).
extern const uint32_t kBankSize;   ///< Its bytes.
}   // namespace battle_alias_bank

namespace
{

using namespace capsule::battle;
namespace ext = capsule::vm_ext;

int g_failures = 0;   ///< Failed checks.
int g_checks = 0;     ///< Checks run.

/// Records one check.
/// @param cond  condition that must hold
#define CHECK(cond)                                                                                                    \
   do                                                                                                                  \
   {                                                                                                                   \
      ++g_checks;                                                                                                      \
      if (!(cond))                                                                                                     \
      {                                                                                                                \
         ++g_failures;                                                                                                 \
         std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
      }                                                                                                                \
   }                                                                                                                   \
   while (false)

constexpr int kVars = 4;          ///< The test's variables.
constexpr int kVarBytes = 8;      ///< Bytes of each.
constexpr uint16_t kStepLimit = 400;   ///< Steps a parity run takes at most.
constexpr uint8_t kNoPoke = 0;    ///< poke's field before it runs.

/// What the commands work on.
struct Context
{
   uint8_t vars[kVars][kVarBytes];   ///< The variables (Variable references 0-3).
   uint8_t poke;                     ///< poke's field.
};

/// A Variable reference's bytes.
/// @param c      the context
/// @param ref    the reference
/// @param bytes  bytes read or written
/// @return them, or nullptr
uint8_t* variable(Context& c, vm::Ref ref, uint16_t bytes)
{
   if (vm::ref_kind(ref) != vm::RefKind::Variable || vm::ref_id(ref) >= kVars ||
       vm::ref_offset(ref) + bytes > kVarBytes)
   {
      return nullptr;
   }
   return &c.vars[vm::ref_id(ref)][vm::ref_offset(ref)];
}

/// The aliases' resolver (Variable references; Code references are the machine's).
/// @param ref      the reference
/// @param bytes    bytes read or written
/// @param context  the Context
/// @return them, or nullptr
uint8_t* resolve_ref(vm::Machine&, vm::Ref ref, uint16_t bytes, void* context)
{
   return variable(*static_cast<Context*>(context), ref, bytes);
}

/// poke: the one native of the manifest.
/// @param m        the machine
/// @param context  the Context
void native_poke(vm::Machine& m, void* context)
{
   static_cast<Context*>(context)->poke = vm::u8_at(m, 1);
   vm::advance(m, 2);
}

// The aliases' dispatch, generated from the manifest (kAliasTable).
#include "battle_alias_dispatch.inc"

/// @name The same commands written as natives (the reference for parity)
///@{

/// Operand offsets and sizes of the layouts.
constexpr uint32_t kOp1 = 1;   ///< First operand.
constexpr uint32_t kOp2 = 2;   ///< After a byte.
constexpr uint32_t kOp5 = 5;   ///< After a word.
constexpr uint32_t kOp6 = 6;   ///< After a byte and a word.
constexpr uint32_t kOp9 = 9;   ///< After two words.
constexpr uint32_t kOp10 = 10; ///< After two words and a byte.
constexpr uint32_t kRefBytes = 4;   ///< A reference or code operand.

/// A pointer operand's bytes, as a game's natives resolve it (a Code reference reads the code).
/// @param m      the machine
/// @param c      the context
/// @param at     the operand's offset
/// @param bytes  bytes read there
/// @return them, or nullptr
uint8_t* memory_at(vm::Machine& m, Context& c, uint32_t at, uint16_t bytes)
{
   const vm::Ref ref = vm::ref_at(m, at);
   if (vm::ref_kind(ref) == vm::RefKind::Code)
   {
      return const_cast<uint8_t*>(vm::code_bytes(m, vm::ref_payload(ref), bytes));
   }
   return variable(c, ref, bytes);
}

/// A code operand's target.
/// @param m   the machine
/// @param at  its offset
/// @return the code offset
uint32_t code_at(vm::Machine& m, uint32_t at)
{
   return vm::ref_payload(vm::ref_at(m, at));
}

/// The jumpif{byte,halfword,word} natives.
/// @param m      the machine
/// @param c      the context
/// @param width  the value's bytes
void jump_if_value(vm::Machine& m, Context& c, uint8_t width)
{
   const uint8_t op = vm::u8_at(m, kOp1);
   const uint32_t value = ext::load(memory_at(m, c, kOp2, width), width);
   const uint32_t with = width == ext::kByte ? vm::u8_at(m, kOp6) : width == ext::kHalf ? vm::u16_at(m, kOp6)
                                                                                        : vm::u32_at(m, kOp6);
   const uint32_t to = code_at(m, kOp6 + width);
   vm::advance(m, kOp6 + width + kRefBytes);
   if (ext::compare(op, value, with))
   {
      vm::jump(m, to);
   }
}

/// The read-modify-write natives.
/// @param m      the machine
/// @param c      the context
/// @param width  the memory's bytes
/// @param alu    the change
void modify(vm::Machine& m, Context& c, uint8_t width, ext::Alu alu)
{
   uint8_t* p = memory_at(m, c, kOp1, width);
   const uint32_t value = width == ext::kByte ? vm::u8_at(m, kOp5) : width == ext::kHalf ? vm::u16_at(m, kOp5)
                                                                                        : vm::u32_at(m, kOp5);
   ext::store(p, width, ext::apply(static_cast<uint8_t>(alu), ext::load(p, width), value));
   vm::advance(m, kOp5 + width);
}

/// The two array compares: jump when the bytes are (equal) or are not (!equal) the same.
/// @param m      the machine
/// @param c      the context
/// @param equal  the sense
void jump_if_array(vm::Machine& m, Context& c, bool equal)
{
   const uint8_t size = vm::u8_at(m, kOp9);
   const uint8_t* a = memory_at(m, c, kOp1, size);
   const uint8_t* b = memory_at(m, c, kOp5, size);
   uint8_t same = 0;
   for (uint8_t i = 0; i < size; i++)
   {
      same = static_cast<uint8_t>(same + (a[i] == b[i] ? 1 : 0));
   }
   if ((same == size) == equal)
   {
      vm::jump(m, code_at(m, kOp10));
   }
   else
   {
      vm::advance(m, kOp10 + kRefBytes);
   }
}

void ref_end(vm::Machine& m, void*)
{
   vm::end(m);
}

void ref_goto(vm::Machine& m, void*)
{
   vm::jump(m, code_at(m, kOp1));
}

void ref_call(vm::Machine& m, void*)
{
   vm::call(m, code_at(m, kOp1), m.ip + kOp1 + kRefBytes);
}

void ref_return(vm::Machine& m, void*)
{
   vm::ret(m);
}

void ref_jumpifbyte(vm::Machine& m, void* c)
{
   jump_if_value(m, *static_cast<Context*>(c), ext::kByte);
}

void ref_jumpifhalfword(vm::Machine& m, void* c)
{
   jump_if_value(m, *static_cast<Context*>(c), ext::kHalf);
}

void ref_jumpifword(vm::Machine& m, void* c)
{
   jump_if_value(m, *static_cast<Context*>(c), ext::kWord);
}

void ref_setbyte(vm::Machine& m, void* c)
{
   modify(m, *static_cast<Context*>(c), ext::kByte, ext::Alu::Set);
}

void ref_addbyte(vm::Machine& m, void* c)
{
   modify(m, *static_cast<Context*>(c), ext::kByte, ext::Alu::Add);
}

void ref_subbyte(vm::Machine& m, void* c)
{
   modify(m, *static_cast<Context*>(c), ext::kByte, ext::Alu::Sub);
}

void ref_orhalfword(vm::Machine& m, void* c)
{
   modify(m, *static_cast<Context*>(c), ext::kHalf, ext::Alu::Or);
}

void ref_bicword(vm::Machine& m, void* c)
{
   modify(m, *static_cast<Context*>(c), ext::kWord, ext::Alu::Bic);
}

void ref_copyarray(vm::Machine& m, void* context)
{
   Context& c = *static_cast<Context*>(context);
   const uint8_t size = vm::u8_at(m, kOp9);
   uint8_t* dest = memory_at(m, c, kOp1, size);
   const uint8_t* src = memory_at(m, c, kOp5, size);
   for (uint8_t i = 0; i < size; i++)
   {
      dest[i] = src[i];
   }
   vm::advance(m, kOp10);
}

void ref_jumpifarrayequal(vm::Machine& m, void* c)
{
   jump_if_array(m, *static_cast<Context*>(c), true);
}

void ref_jumpifarraynotequal(vm::Machine& m, void* c)
{
   jump_if_array(m, *static_cast<Context*>(c), false);
}

void ref_pause(vm::Machine& m, void*)
{
   if (vm::pause(m, vm::u16_at(m, kOp1)))
   {
      vm::advance(m, kOp1 + ext::kHalf);
   }
}

void ref_waitflag(vm::Machine& m, void* context)
{
   uint8_t& flag = static_cast<Context*>(context)->vars[3][0];
   if (flag == 0)
   {
      vm::advance(m, kOp1 + ext::kHalf);
   }
   else if (vm::pause(m, vm::u16_at(m, kOp1)))
   {
      vm::advance(m, kOp1 + ext::kHalf);
      flag = 0;
   }
}

void ref_waituntil(vm::Machine& m, void* context)
{
   Context& c = *static_cast<Context*>(context);
   if (*memory_at(m, c, kOp1, ext::kByte) == vm::u8_at(m, kOp5))
   {
      vm::advance(m, kOp6);
   }
   else
   {
      vm::yield(m);
   }
}

void ref_hold(vm::Machine&, void*)
{
}

void ref_yieldnop(vm::Machine& m, void*)
{
   vm::yield(m);
   vm::advance(m, kOp1);
}

void ref_choose(vm::Machine& m, void* context)
{
   const Context& c = *static_cast<Context*>(context);
   vm::jump(m, (c.vars[0][0] & 1u) != 0 ? code_at(m, kOp5) : code_at(m, kOp1));
}

void ref_signedadd(vm::Machine& m, void* context)
{
   uint8_t* p = memory_at(m, *static_cast<Context*>(context), kOp1, ext::kHalf);
   const uint32_t v = static_cast<uint32_t>(static_cast<int32_t>(vm::s8_at(m, kOp5)));
   ext::store(p, ext::kHalf, ext::load(p, ext::kHalf) + v);
   vm::advance(m, kOp6);
}

void ref_nop(vm::Machine& m, void*)
{
   vm::advance(m, kOp1);
}
///@}

/// The natives by id (the manifest's ids).
constexpr vm::Command kNatives[] = {ref_end,       ref_goto,           ref_call,           ref_return,
                                    ref_jumpifbyte, ref_jumpifhalfword, ref_jumpifword,     ref_setbyte,
                                    ref_addbyte,   ref_subbyte,        ref_orhalfword,     ref_bicword,
                                    ref_copyarray, ref_jumpifarrayequal, ref_jumpifarraynotequal, ref_pause,
                                    ref_waitflag,  ref_waituntil,      ref_hold,           ref_yieldnop,
                                    ref_choose,    ref_signedadd,      ref_nop,            native_poke};
/// The natives' table.
constexpr vm::CommandTable kNativeTable = {kNatives, sizeof(kNatives) / sizeof(kNatives[0]), nullptr, nullptr};
static_assert(sizeof(kNatives) / sizeof(kNatives[0]) == 24);

/// Two machines in the same state.
/// @param a  one
/// @param b  the other
/// @return true when position, stack, status, fault, yield and pause counter agree
bool same_machine(const vm::Machine& a, const vm::Machine& b)
{
   bool same = a.ip == b.ip && a.depth == b.depth && a.status == b.status && a.fault == b.fault &&
               a.yielded == b.yielded && a.pause_frames == b.pause_frames;
   for (uint8_t i = 0; i < a.depth && same; ++i)
   {
      same = a.stack[i] == b.stack[i];
   }
   return same;
}

/// Runs a label on both tables, step by step, comparing the machines and contexts after each step.
/// @param bank   the bank
/// @param label  where to start
/// @param a      the aliases' context
/// @param n      the natives' context
/// @return steps run until both stopped (or kStepLimit)
uint16_t parity_run(const vm::Bank& bank, uint16_t label, Context& a, Context& n)
{
   vm::Machine ma{};
   vm::Machine mn{};
   vm::reset(ma, bank);
   vm::reset(mn, bank);
   vm::start_label(ma, label);
   vm::start_label(mn, label);
   uint16_t steps = 0;
   while (steps < kStepLimit && ma.status == vm::Status::Running)
   {
      vm::step(ma, kAliasTable, &a);
      vm::step(mn, kNativeTable, &n);
      ++steps;
      if (!same_machine(ma, mn) || std::memcmp(&a, &n, sizeof(a)) != 0)
      {
         std::printf("parity: label %u differs after step %u (ip %u vs %u)\n", label, steps, ma.ip, mn.ip);
         CHECK(false);
         break;
      }
   }
   return steps;
}

/// The whole bank: aliases and natives agree on every step; the script ends where expected.
/// @param bank  the bank
void test_parity(const vm::Bank& bank)
{
   Context a{};
   Context n{};
   const uint16_t steps = parity_run(bank, battle_alias_bank::kLabelMain, a, n);
   CHECK(steps > 0 && steps < kStepLimit);
   CHECK(a.vars[0][0] == 3);         // Fin's setbyte
   CHECK(a.vars[1][0] == 0x30 && a.vars[1][1] == 0x10);   // 0x1234 & ~0x0204
   CHECK(a.vars[2][1] == 9);         // copied, then changed
   CHECK(a.vars[2][4] == 0xFD && a.vars[2][5] == 0xFF);   // a signed add of -3 on a halfword
   CHECK(a.vars[3][0] == 0);         // waitflag cleared it
   CHECK(a.poke == 7);
   // Main ends at Wait (variable 3 byte 1 is 0, not 1): the wait yields until the test sets it
   vm::Machine m{};
   vm::reset(m, bank);
   Context c{};
   vm::start_label(m, battle_alias_bank::kLabelWait);
   CHECK(vm::step(m, kAliasTable, &c) == vm::Result::Ran);
   CHECK(m.yielded && m.ip == vm::label(bank, battle_alias_bank::kLabelWait));
   c.vars[3][1] = 1;
   vm::step(m, kAliasTable, &c);
   CHECK(!m.yielded && m.ip != vm::label(bank, battle_alias_bank::kLabelWait));
   CHECK(c.poke == kNoPoke);
}

/// Steps timed and held: pause counts frames; hold never moves.
/// @param bank  the bank
void test_waits(const vm::Bank& bank)
{
   vm::Machine m{};
   vm::reset(m, bank);
   Context c{};
   vm::start_label(m, battle_alias_bank::kLabelSpin);
   const uint32_t at = m.ip;
   for (int i = 0; i < kVarBytes; ++i)
   {
      vm::step(m, kAliasTable, &c);
   }
   CHECK(m.ip == at && m.status == vm::Status::Running);
}

/// The faults: a reference nobody resolves, a return with an empty stack, a command past the code, a step past
/// the table, a step the machine does not know.
/// @param bank  the bank
void test_faults(const vm::Bank& bank)
{
   Context c{};
   vm::Machine m{};
   vm::reset(m, bank);
   vm::start_label(m, battle_alias_bank::kLabelBadRef);
   vm::step(m, kAliasTable, &c);
   CHECK(m.status == vm::Status::Fault && m.fault == vm::Fault::BadRef);
   vm::start_label(m, battle_alias_bank::kLabelUnderflow);
   vm::step(m, kAliasTable, &c);
   CHECK(m.status == vm::Status::Fault && m.fault == vm::Fault::StackUnderflow);
   vm::start_label(m, battle_alias_bank::kLabelTail);
   vm::step(m, kAliasTable, &c);
   CHECK(m.status == vm::Status::Fault && m.fault == vm::Fault::OutOfCode);
   // hand-made: an alias past its step table, then a step of no known kind
   static constexpr ext::Step kBad[] = {{ext::Op::NUM_OPS, 0, 0, 0, {}}};
   constexpr vm::AliasSet kBadSet = {kBad, 1, nullptr};
   vm::start_label(m, battle_alias_bank::kLabelMain);
   vm::run_alias(m, ext::Alias{1, 1, 1}, kBadSet, &c);
   CHECK(m.status == vm::Status::Fault && m.fault == vm::Fault::BadStep);
   vm::start_label(m, battle_alias_bank::kLabelMain);
   vm::run_alias(m, ext::Alias{0, 1, 1}, kBadSet, &c);
   CHECK(m.status == vm::Status::Fault && m.fault == vm::Fault::BadStep);
}

/// The shared vocabulary's helpers.
void test_vocabulary()
{
   CHECK(ext::compare(static_cast<uint8_t>(ext::Compare::Greater), 3, 2));
   CHECK(!ext::compare(static_cast<uint8_t>(ext::Compare::Less), 3, 2));
   CHECK(!ext::compare(static_cast<uint8_t>(ext::Compare::NUM_COMPARES), 1, 1));
   CHECK(ext::apply(static_cast<uint8_t>(ext::Alu::Bic), 0xFF, 0x0F) == 0xF0);
   CHECK(ext::apply(static_cast<uint8_t>(ext::Alu::NUM_ALUS), 5, 1) == 5);
   constexpr uint8_t kSigned[] = {0xFE, 0xFF};
   CHECK(ext::decode(kSigned, ext::Type::S8) == 0xFFFFFFFEu);
   CHECK(ext::decode(kSigned, ext::Type::S16) == 0xFFFFFFFEu);
   CHECK(ext::decode(kSigned, ext::Type::U16) == 0xFFFEu);
   uint8_t word[ext::kWord] = {};
   ext::store(word, ext::kWord, 0x01020304u);
   CHECK(ext::load(word, ext::kWord) == 0x01020304u && word[0] == 4);
}

}   // namespace

/// Runs the tests.
/// @return 0 when every check passed
int main()
{
   vm::Bank bank{};
   CHECK(vm::open(bank, battle_alias_bank::kBank, battle_alias_bank::kBankSize) == vm::BankError::None);
   test_vocabulary();
   test_parity(bank);
   test_waits(bank);
   test_faults(bank);
   std::printf("battle_vm_alias_tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
