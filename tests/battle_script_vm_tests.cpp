/// @file
/// Tests for the battle script machine (capsule/battle/vm.h), the controller channel (capsule/battle/controller.h)
/// and the raw draws (capsule/core/random_raw.h), M5 BX. The bank is tests/data/battle_vm/test_bank.json, assembled
/// at build time by tools/script_bank.py (test_bank.cpp, test_bank_ids.h), so the writer and the reader are tested
/// together; hand-made bytes test the refusals.
#include "capsule/battle/controller.h"
#include "capsule/battle/vm.h"
#include "capsule/core/random_raw.h"

#include "test_bank_ids.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace test_bank
{
extern const uint8_t kBank[];      ///< The assembled bank (test_bank.cpp).
extern const uint32_t kBankSize;   ///< Its bytes.
}   // namespace test_bank

namespace
{

using namespace capsule::battle;

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

constexpr uint8_t kEndpoints = 2;          ///< The test channel's endpoints.
constexpr uint16_t kBufferBytes = 16;      ///< Its buffers' bytes.
constexpr int kVars = 4;                   ///< The test's variables.
constexpr int kVarBytes = 4;               ///< Bytes of each.

/// What the test commands work on.
struct Context
{
   int acc;                                          ///< add's accumulator.
   uint8_t vars[kVars][kVarBytes];                   ///< set's variables.
   controller::Buffers<kEndpoints, kBufferBytes> buffers;   ///< send's channel.
   controller::Channel channel;                      ///< Its view.
   uint32_t list_sum;                                ///< list's sum of its data.
   int traced;                                       ///< Commands the trace saw.
};

/// @name The test command set (test_bank.json's ids)
///@{
void cmd_nop(vm::Machine& m, void*)
{
   vm::advance(m, 1);
}

void cmd_end(vm::Machine& m, void*)
{
   vm::end(m);
}

void cmd_goto(vm::Machine& m, void*)
{
   vm::jump(m, vm::ref_payload(vm::ref_at(m, 1)));
}

void cmd_call(vm::Machine& m, void*)
{
   vm::call(m, vm::ref_payload(vm::ref_at(m, 1)), m.ip + 5);
}

void cmd_return(vm::Machine& m, void*)
{
   vm::ret(m);
}

void cmd_send(vm::Machine& m, void* context)
{
   Context& c = *static_cast<Context*>(context);
   const uint8_t payload[] = {vm::u8_at(m, 2)};
   controller::send(c.channel, vm::u8_at(m, 1), payload, sizeof(payload));
   vm::advance(m, 3);
}

void cmd_pause(vm::Machine& m, void*)
{
   if (vm::pause(m, vm::u16_at(m, 1)))
   {
      vm::advance(m, 3);
   }
}

void cmd_set(vm::Machine& m, void* context)
{
   Context& c = *static_cast<Context*>(context);
   const vm::Ref r = vm::ref_at(m, 1);
   if (vm::ref_kind(r) == vm::RefKind::Variable && vm::ref_id(r) < kVars && vm::ref_offset(r) < kVarBytes)
   {
      c.vars[vm::ref_id(r)][vm::ref_offset(r)] = vm::u8_at(m, 5);
   }
   vm::advance(m, 6);
}

void cmd_yield(vm::Machine& m, void*)
{
   vm::yield(m);
   vm::advance(m, 1);
}

void cmd_add(vm::Machine& m, void* context)
{
   static_cast<Context*>(context)->acc += vm::s8_at(m, 1);
   vm::advance(m, 2);
}

void cmd_list(vm::Machine& m, void* context)
{
   Context& c = *static_cast<Context*>(context);
   const uint32_t at = vm::ref_payload(vm::ref_at(m, 1));
   const uint8_t* count = vm::code_bytes(m, at, 1);
   const uint8_t* items = count != nullptr ? vm::code_bytes(m, at + 1, *count * 2u) : nullptr;
   for (uint8_t i = 0; items != nullptr && i < *count; ++i)
   {
      c.list_sum += static_cast<uint32_t>(items[i * 2] | (items[i * 2 + 1] << 8));
   }
   vm::advance(m, 5);
}
///@}

/// The table (ids as test_bank.json).
constexpr vm::Command kCommands[] = {cmd_nop, cmd_end, cmd_goto, cmd_call, cmd_return, cmd_send,
                                     cmd_pause, cmd_set, cmd_yield, cmd_add, cmd_list};

/// Counts the traced commands.
void trace(const vm::Machine&, uint8_t, void* context)
{
   ++static_cast<Context*>(context)->traced;
}

/// A fresh context with its channel.
/// @param c  the context
void init(Context& c)
{
   std::memset(&c, 0, sizeof(c));
   c.channel = c.buffers.view();
}

void test_bank_reading()
{
   vm::Bank bank;
   CHECK(vm::open(bank, test_bank::kBank, test_bank::kBankSize) == vm::BankError::None);
   CHECK(bank.label_count == 6);
   CHECK(vm::label(bank, test_bank::kLabelMain) == 0);
   CHECK(vm::label(bank, test_bank::kLabelTwice) == 32);
   CHECK(vm::label(bank, test_bank::kLabelData) == 42);
   CHECK(vm::label(bank, test_bank::kLabelEnd) == bank.code_bytes);
   CHECK(std::strcmp(vm::label_name(bank, test_bank::kLabelTwice), "Twice") == 0);
   CHECK(vm::label_name(bank, 99) == nullptr);
   CHECK(vm::label_at(bank, 32) == test_bank::kLabelTwice);
   CHECK(vm::label_at(bank, 33) == vm::kNoLabel);
   CHECK(vm::label_before(bank, 35) == test_bank::kLabelTwice);
   CHECK(vm::label_before(bank, 0) == test_bank::kLabelMain);

   // refusals: short, magic, version, a label past the code
   uint8_t copy[256];
   CHECK(test_bank::kBankSize <= sizeof(copy));
   std::memcpy(copy, test_bank::kBank, test_bank::kBankSize);
   CHECK(vm::open(bank, copy, vm::kHeaderBytes - 1) == vm::BankError::TooSmall);
   CHECK(bank.data == nullptr);
   copy[0] ^= 1;
   CHECK(vm::open(bank, copy, test_bank::kBankSize) == vm::BankError::BadMagic);
   copy[0] ^= 1;
   copy[4] = 2;
   CHECK(vm::open(bank, copy, test_bank::kBankSize) == vm::BankError::BadVersion);
   copy[4] = 1;
   copy[vm::kHeaderBytes] = 0xFF;   // the first label's offset (Main) far past the code
   CHECK(vm::open(bank, copy, test_bank::kBankSize) == vm::BankError::BadLayout);
   CHECK(vm::open(bank, copy, 0) == vm::BankError::TooSmall);
}

void test_references()
{
   const vm::Ref r = vm::make_ref(vm::RefKind::Table, 0x123, 0x4567);
   CHECK(vm::ref_kind(r) == vm::RefKind::Table);
   CHECK(vm::ref_id(r) == 0x123);
   CHECK(vm::ref_offset(r) == 0x4567);
   CHECK(vm::ref_kind(vm::code_ref(77)) == vm::RefKind::Code);
   CHECK(vm::ref_payload(vm::code_ref(77)) == 77);
}

void test_running()
{
   vm::Bank bank;
   vm::open(bank, test_bank::kBank, test_bank::kBankSize);
   Context c;
   init(c);
   const vm::CommandTable table{kCommands, sizeof(kCommands) / sizeof(kCommands[0]), nullptr, trace};
   vm::Machine m;
   vm::reset(m, bank);
   vm::wait_on(m, c.channel.exec);
   CHECK(vm::step(m, table, &c) == vm::Result::Stopped);   // not started
   vm::start_label(m, test_bank::kLabelMain);

   // add 1, call Twice (add 2, add 2, return), add -3: one command a step
   for (int i = 0; i < 6; ++i)
   {
      CHECK(vm::step(m, table, &c) == vm::Result::Ran);
   }
   CHECK(c.acc == 2);
   CHECK(m.depth == 0);
   CHECK(vm::step(m, table, &c) == vm::Result::Ran);   // set
   CHECK(c.vars[2][1] == 7);
   CHECK(vm::step(m, table, &c) == vm::Result::Ran);   // send 1, 42
   CHECK(controller::pending(c.channel, 1));
   CHECK(controller::command(c.channel, 1)[0] == 42);
   CHECK(vm::blocked(m));
   const uint32_t ip = m.ip;
   CHECK(vm::step(m, table, &c) == vm::Result::Blocked);
   CHECK(m.ip == ip);
   controller::complete(c.channel, 1);
   CHECK(vm::step(m, table, &c) == vm::Result::Ran);   // add 10
   CHECK(c.acc == 12);

   // pause 3: the third step goes on
   CHECK(vm::step(m, table, &c) == vm::Result::Ran);
   CHECK(vm::step(m, table, &c) == vm::Result::Ran);
   CHECK(m.ip == ip + 2);   // still on the pause (add 10 took the 2 bytes after the send)
   CHECK(vm::step(m, table, &c) == vm::Result::Ran);
   CHECK(m.ip == ip + 5);
   CHECK(m.pause_frames == 0);

   // run(): add 100, yield (stops), then list, end
   CHECK(vm::run(m, table, &c, 10) == 2);
   CHECK(c.acc == 112);
   CHECK(vm::run(m, table, &c, 10) == 2);
   CHECK(c.list_sum == 513u + 1027u + 1541u);
   CHECK(m.status == vm::Status::Ended);
   CHECK(vm::step(m, table, &c) == vm::Result::Stopped);
   CHECK(c.traced == 16);   // the blocked step ran nothing
}

void test_faults()
{
   vm::Bank bank;
   vm::open(bank, test_bank::kBank, test_bank::kBankSize);
   Context c;
   init(c);
   const vm::CommandTable table{kCommands, sizeof(kCommands) / sizeof(kCommands[0]), nullptr, nullptr};
   vm::Machine m;
   vm::reset(m, bank);

   // a call to itself overflows the stack at its ninth call
   vm::start_label(m, test_bank::kLabelRecurse);
   CHECK(vm::run(m, table, &c, 20) == vm::kStackDepth + 1);
   CHECK(m.status == vm::Status::Fault);
   CHECK(m.fault == vm::Fault::StackOverflow);

   // an id the table does not have
   vm::start_label(m, test_bank::kLabelUnknown);
   CHECK(vm::step(m, table, &c) == vm::Result::Ran);
   CHECK(m.fault == vm::Fault::NoCommand);
   // ... with an unknown-command function, it runs that instead
   const vm::CommandTable lenient{kCommands, sizeof(kCommands) / sizeof(kCommands[0]), cmd_nop, nullptr};
   vm::start_label(m, test_bank::kLabelUnknown);
   CHECK(vm::step(m, lenient, &c) == vm::Result::Ran);
   CHECK(m.status == vm::Status::Running);
   CHECK(m.ip == vm::label(bank, test_bank::kLabelEnd));
   // at the code's end there is no command
   CHECK(vm::step(m, lenient, &c) == vm::Result::Stopped);
   CHECK(m.fault == vm::Fault::OutOfCode);

   // an operand past the code: Data's u16s read as add / goto operands at its end
   vm::start(m, vm::label(bank, test_bank::kLabelEnd) - 1);
   CHECK(vm::u32_at(m, 1) == 0);
   CHECK(m.fault == vm::Fault::OutOfCode);

   // return with nothing pushed
   vm::start_label(m, test_bank::kLabelTwice);
   vm::run(m, table, &c, 3);
   CHECK(m.fault == vm::Fault::StackUnderflow);

   // a start past the code
   vm::start(m, bank.code_bytes + 1);
   CHECK(m.status == vm::Status::Fault);
   vm::stop(m);
   CHECK(m.status == vm::Status::Idle);
}

/// A handler that answers with its command's second byte plus one and finishes.
void handler_echo(uint8_t endpoint, void* context)
{
   Context& c = *static_cast<Context*>(context);
   const uint8_t answer[] = {static_cast<uint8_t>(controller::command(c.channel, endpoint)[1] + 1)};
   controller::write_reply(c.channel, endpoint, answer, sizeof(answer));
   controller::complete(c.channel, endpoint);
}

/// A handler that keeps the command running (its step takes over).
void handler_slow(uint8_t endpoint, void* context)
{
   Context& c = *static_cast<Context*>(context);
   ++c.acc;
   static_cast<void>(endpoint);
}

/// An idle step: dispatches with the echo table.
void step_idle(uint8_t endpoint, void* context)
{
   static constexpr controller::Handler kHandlers[] = {handler_echo, handler_slow};
   static constexpr controller::HandlerTable kTable{kHandlers, sizeof(kHandlers) / sizeof(kHandlers[0]), nullptr};
   controller::dispatch(static_cast<Context*>(context)->channel, endpoint, kTable, context);
}

void test_channel()
{
   Context c;
   init(c);
   controller::Channel& ch = c.channel;
   CHECK(controller::bit(0) == 1u);
   CHECK(controller::bit(3, 7) == (1u << 31));
   CHECK(controller::lane_bits(1) == 0xF0u);
   CHECK(!controller::busy(ch));

   // writes keep the bytes past their size, and are clipped to the buffer
   const uint8_t long_data[kBufferBytes + 4] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
   controller::write_command(ch, 0, long_data, sizeof(long_data));
   CHECK(controller::command(ch, 0)[kBufferBytes - 1] == 9);
   CHECK(controller::command(ch, 1)[0] == 0);   // the next endpoint's buffer is untouched
   const uint8_t echo[] = {0, 41};
   controller::send(ch, 0, echo, sizeof(echo));
   CHECK(controller::command(ch, 0)[2] == 9);
   CHECK(controller::pending(ch, 0));
   CHECK(controller::busy(ch, controller::lane_bits(controller::kLocalLane)));
   CHECK(!controller::busy(ch, controller::lane_bits(1)));

   // a frame of the presentation side: the idle steps in endpoint order
   controller::set_step(ch, 0, step_idle);
   controller::set_step(ch, 1, step_idle);
   uint8_t active = controller::kNoEndpoint;
   controller::run(ch, kEndpoints, &c, &active);
   CHECK(active == kEndpoints);
   CHECK(controller::reply(ch, 0)[0] == 42);
   CHECK(!controller::busy(ch));

   // a slow command stays pending; an unknown one completes at once
   const uint8_t slow[] = {1};
   controller::send(ch, 1, slow, sizeof(slow));
   controller::run(ch, kEndpoints, &c);
   CHECK(c.acc == 1);
   CHECK(controller::pending(ch, 1));
   const uint8_t unknown[] = {7};
   controller::send(ch, 0, unknown, sizeof(unknown));
   controller::run(ch, 1, &c);
   CHECK(!controller::pending(ch, 0));
   CHECK(controller::pending(ch, 1));

   // other lanes are the game's
   controller::mark(ch, 2, 7);
   CHECK(controller::pending(ch, 2, 7));
   CHECK(!controller::pending(ch, 2));
   controller::complete(ch, 2, 7);
   controller::reset(ch);
   CHECK(!controller::busy(ch));
   CHECK(ch.steps[0] == nullptr);
   CHECK(controller::command(ch, 0)[2] == 9);   // reset keeps the bytes
   controller::clear_buffers(ch);
   CHECK(controller::command(ch, 0)[2] == 0);
}

void test_raw_draws()
{
   using namespace capsule::rng;
   Stream a{0x1234u};
   Stream b{0x1234u};
   for (uint16_t n = 1; n < 300; n += 7)
   {
      CHECK(draw_mod(a, n) == next16(b) % n);
      CHECK(draw_mask(a, n) == (next16(b) & n));
      CHECK(draw(a) == next16(b));
   }
   CHECK(a.state == b.state);
   // a zero divisor still draws once and keeps the draw
   const uint16_t kept = draw_mod(a, 0);
   CHECK(kept == next16(b));
   CHECK(a.state == b.state);
   // the gameplay stream's overloads
   gameplay.state = 0x55u;
   Stream copy{0x55u};
   CHECK(draw_mod(100) == next16(copy) % 100);
   CHECK(draw_mask(0xFF) == (next16(copy) & 0xFF));
   CHECK(draw() == next16(copy));
}

} // namespace

/// Runs the checks.
/// @return 0 when all pass
int main()
{
   test_bank_reading();
   test_references();
   test_running();
   test_faults();
   test_channel();
   test_raw_draws();
   std::printf("battle script vm tests: %d checks, %d failures\n", g_checks, g_failures);
   return g_failures == 0 ? 0 : 1;
}
