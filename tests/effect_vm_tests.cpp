/// @file
/// Tests for the effect VM (capsule/battle/effect.h, src/battle/effect_vm.cpp) with a mock host
/// (tests/support/effect_mock_host.h): every program of tests/data/effects/vm_ops.ceffect (every opcode the compiler
/// emits), hand-assembled programs for the opcodes it does not emit and for every fault, yield / ask / resume,
/// nesting and the depth limit, determinism of the draws and the generated point handlers. The D-23 section 1.7
/// Classic examples run against the same mock host in the reference-ruleset repo (E11).
#include "capsule/battle/effect.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vm_effects
{
extern const capsule::battle::effect::Bank kBank;
extern const char* const kOwnerNames[];
int32_t point_handler(uint16_t fn, capsule::battle::effect::PointCtx& c, int32_t v);
} // namespace vm_effects

using namespace capsule::battle;
using namespace capsule::battle::effect;

#include "support/effect_mock_host.h"

namespace
{

/// Runs a neutral-fixture hook program on a value.
/// @param owner  "kind Name"
/// @param hook   the hook
/// @param value  ctx.value in
/// @param st     the status out
/// @return ctx.value out
int32_t run_hook(const char* owner, Hook hook, int32_t value, Status* st = nullptr)
{
   Context ctx = context(value);
   const Status s = invoke(g_machine, vm_effects::kBank, g_host,
                           find(vm_effects::kBank, vm_effects::kOwnerNames, owner, hook), ctx);
   if (st != nullptr)
   {
      *st = s;
   }
   return ctx.value;
}

/// Arithmetic, comparisons, logic, Q8, loops, subs (neutral fixture).
void test_values()
{
   reset(vm_effects::kBank);
   CHECK_EQ(run_hook("global arith", Hook::BeforeDamage, 10), 43);
   CHECK_EQ(run_hook("global unary", Hook::BeforeDamage, 10), 70);
   CHECK_EQ(run_hook("global saturate", Hook::BeforeDamage, 0), INT32_MAX);
   CHECK_EQ(run_hook("global negative", Hook::BeforeDamage, 0), INT32_MIN);
   CHECK_EQ(run_hook("global div_zero", Hook::BeforeDamage, 10), 7);
   CHECK_EQ(run_hook("global compare", Hook::BeforeDamage, 10), 21);
   CHECK_EQ(run_hook("global compare", Hook::BeforeDamage, 11), 2 + 16 + 32);
   CHECK_EQ(run_hook("global logic", Hook::BeforeDamage, 10), 3);
   CHECK_EQ(run_hook("global logic", Hook::BeforeDamage, 3), 6);
   CHECK_EQ(run_hook("global q8", Hook::BeforeDamage, 10), 79);
   CHECK_EQ(run_hook("global loop", Hook::BeforeDamage, 10), 10);   // repeat clamps to max 5
   CHECK_EQ(run_hook("global loop", Hook::BeforeDamage, 2), 4);
   CHECK_EQ(run_hook("global loop", Hook::BeforeDamage, 0), 0);
   CHECK_EQ(run_hook("global subs", Hook::BeforeDamage, 10), 28);
   CHECK_EQ(run_hook("global early", Hook::BeforeDamage, 0), 1);
   CHECK_EQ(g_machine.faults, 0);
}

/// Get / Has / Subj with every subject kind, the battler loop, writes and roles.
void test_reads_writes()
{
   reset(vm_effects::kBank);
   const Place holder{PlaceKind::Battler, 0, 6};
   const Place other{PlaceKind::Battler, 1, 6};
   const Place user{PlaceKind::Battler, 2, 6};
   const Place target{PlaceKind::Battler, 3, 6};
   const Place user_side{PlaceKind::Side, 2, 6};
   const Place holder_side{PlaceKind::Side, 0, 6};
   const Place move{PlaceKind::Move, 0, 6};
   const Place field{PlaceKind::Field, 0, 6};
   const Place none{PlaceKind::None, 0, 6};
   int32_t want = formula(Ref::Hp, holder, 0) + formula(Ref::Level, other, 0) + formula(Ref::Stat, user, 1) +
                  formula(Ref::Stage, target, 1) + formula(Ref::SideLayers, user_side, 0) +
                  formula(Ref::MovePower, move, 0);
   want += formula(Ref::MoveField, move, 0) + formula(Ref::WeatherTurns, field, 0) + formula(Ref::FieldTurns, field, 0) +
           formula(Ref::Turn, none, 0) + formula(Ref::Hits, none, 0) + formula(Ref::Crit, none, 0);
   want += formula(Ref::SpeciesField, holder, 0) + formula(Ref::Type, holder, 1) + formula(Ref::TakenAmount, holder, 0) +
           6 + 9;
   CHECK_EQ(run_hook("global reads", Hook::BeforeDamage, 0), want);

   int32_t has = 0;
   has += has_formula(HasKind::Volatile, holder, 2) ? 1 : 0;
   has += has_formula(HasKind::Tag, other, 0) ? 2 : 0;
   has += has_formula(HasKind::Status, user, 0) ? 4 : 0;
   has += has_formula(HasKind::Type, target, 1) ? 8 : 0;
   has += has_formula(HasKind::MoveFlag, move, 1) ? 16 : 0;
   has += has_formula(HasKind::SideCond, holder_side, 0) ? 32 : 0;
   has += has_formula(HasKind::FieldCond, field, 0) ? 64 : 0;
   has += 128 + 512;   // other != holder; user != holder; is_foe(holder, other): sides from the formula differ
   CHECK_EQ(run_hook("global has", Hook::BeforeDamage, 0), has);

   // foes(0) = {1, 3}; all(0) = {0, 1, 2, 3}, lowest first
   int32_t levels = 0;
   for (uint8_t b = 0; b < 4; ++b)
   {
      levels += formula(Ref::Level, Place{PlaceKind::Battler, b, 6}, 0);
   }
   CHECK_EQ(run_hook("global each", Hook::BeforeDamage, 0), 24 * 1000 + levels);

   CHECK_EQ(run_hook("global writes", Hook::BeforeDamage, 0), 87);
   CHECK_EQ(g_host.counters[0][0], 10);
   CHECK_EQ(g_host.words[0][0], 300);
   CHECK_EQ(g_host.status_counter[0], 2);
   CHECK_EQ(g_host.status_aux[0], -1);   // the host clamps to its storage; the mock keeps the raw value
   CHECK_EQ(g_host.roles[0][2], 77);
   CHECK_EQ(g_machine.faults, 0);
}

/// Draws: values, order, the no-draw cases and determinism.
void test_rng()
{
   reset(vm_effects::kBank);
   HostState replay{};
   replay.seed = 7;
   const int32_t r6 = static_cast<int32_t>(next_draw(replay) % 6);
   const int32_t r25 = 2 + static_cast<int32_t>(next_draw(replay) % 4);
   const int32_t r12 = next_draw(replay) % 2 < 1 ? 1 : 0;
   const int32_t first = run_hook("global rng", Hook::BeforeDamage, 0);
   CHECK_EQ(first, r6 * 100 + r25 * 10 + r12 + 5000);
   CHECK_EQ(g_host.draws, 3);   // rand(0), between(5, 2), chance(0, 3) and chance(1, 0) draw nothing
   reset(vm_effects::kBank);
   CHECK_EQ(run_hook("global rng", Hook::BeforeDamage, 0), first);   // same seed, same result
   g_host.seed = 1234;
   const int32_t other = run_hook("global rng", Hook::BeforeDamage, 0);
   CHECK(other >= 5000 && other < 5000 + 600);
}

/// Effect odds (Op::Odds, E9): the n m by on operands reach the host in order, and odds that cannot succeed draw
/// nothing (the mock host's odds are plain: draw % m < n).
void test_odds()
{
   reset(vm_effects::kBank);
   HostState replay{};
   replay.seed = 7;
   const int32_t a = next_draw(replay) % 2 < 1 ? 1 : 0;
   const int32_t b = next_draw(replay) % 2 < 1 ? 1 : 0;
   const int32_t c = next_draw(replay) % 5 < 5 ? 1 : 0;
   CHECK_EQ(run_hook("global odds", Hook::BeforeDamage, 0), a + b * 2 + c * 8);
   CHECK_EQ(g_host.draws, 3);   // odds(0, 3) draws nothing
   CHECK_EQ(g_machine.faults, 0);
}

/// Checks one logged primitive.
/// @param i      log index
/// @param op     opcode
/// @param kind   subject kind
/// @param who    subject battler
/// @param id     id immediate
/// @param flags  flags immediate
/// @param arg    first operand (or the operand count is 0 when argc is 0)
/// @param argc   operands
/// @param line   source line
void check_log(int i, Op op, PlaceKind kind, int who, int id, int flags, int32_t arg, int argc, int line)
{
   const Action& a = g_host.log[i].action;
   ++g_checks;
   const bool ok = a.op == op && a.at.kind == kind && a.at.battler == who && a.id == id && a.flags == flags &&
                   a.argc == argc && (argc == 0 || a.args[0] == arg);
   if (!ok)
   {
      std::printf("FAIL line %d: log[%d] op %d kind %d who %d id %d flags %d argc %d arg %d\n", line, i,
                  static_cast<int>(a.op), static_cast<int>(a.at.kind), a.at.battler, a.id, a.flags, a.argc,
                  a.argc ? a.args[0] : 0);
      ++g_failures;
   }
}

#define LOG(i, op, kind, who, id, flags, arg, argc) check_log(i, op, kind, who, id, flags, arg, argc, __LINE__)

/// Every primitive, in order, with a nested run (Inflict -> the status's start hook).
void test_primitives()
{
   reset(vm_effects::kBank);
   g_host.marked_start = find(vm_effects::kBank, vm_effects::kOwnerNames, "status Marked", Hook::Start);
   EffectRun r{};
   begin(r, find(vm_effects::kBank, vm_effects::kOwnerNames, "move Everything", Hook::Use), context(0), true);
   CHECK(resume(g_machine, vm_effects::kBank, r, g_host) == Status::Done);
   const PlaceKind b = PlaceKind::Battler;
   const PlaceKind n = PlaceKind::None;
   int i = 0;
   LOG(i++, Op::Heal, b, 2, 0, 0, 7, 1);
   LOG(i++, Op::Inflict, b, 3, 0, kInflictQuiet, 0, 0);
   LOG(i++, Op::Text, n, 0, 1, 0, 0, 0);   // nested: status Marked on start
   CHECK_EQ(g_host.log[i - 1].depth, 2);
   LOG(i++, Op::Cure, b, 3, 0, 0, 0, 0);
   LOG(i++, Op::Stage, b, 3, 0, 0, -2, 1);
   LOG(i++, Op::VolOn, b, 2, 2, 0, 3, 1);
   LOG(i++, Op::VolOff, b, 2, 2, 0, 0, 0);
   LOG(i++, Op::SideOn, PlaceKind::Side, 3, 0, kSideAddLayer, 0, 1);
   LOG(i++, Op::SideOff, PlaceKind::Side, 3, 0, 0, 0, 0);
   LOG(i++, Op::FieldOn, n, 0, 0, 0, 4, 1);
   LOG(i++, Op::FieldOff, n, 0, 0, 0, 0, 0);
   LOG(i++, Op::Weather, n, 0, 0, 0, 5, 1);
   LOG(i++, Op::Lock, b, 2, 0, kLockFree, 0, 0);
   LOG(i++, Op::Unlock, b, 2, 0, 0, 0, 0);
   LOG(i++, Op::UseMove, b, 2, 0, 0, 1, 1);
   LOG(i++, Op::Credit, n, 0, 0, 0, 11, 1);
   LOG(i++, Op::ConsumeItem, b, 3, 0, 0, 0, 0);
   LOG(i++, Op::SetItem, b, 3, 0, 0, 0, 1);
   LOG(i++, Op::SetPassive, b, 3, 0, 0, 0, 1);
   LOG(i++, Op::SetTypes, b, 3, 0, 2, 1, 2);
   CHECK_EQ(g_host.log[i - 1].action.args[1], 2);
   LOG(i++, Op::Text, n, 0, 0, 0, 0, 0);
   LOG(i++, Op::Say, n, 0, 0, 0, 0, 0);
   LOG(i++, Op::SayAt, b, 2, 0, 0, 3, 1);
   CHECK(g_host.log[i - 1].action.about.kind == b && g_host.log[i - 1].action.about.battler == 3);
   LOG(i++, Op::Announce, n, 0, 0, 0, 0, 0);
   LOG(i++, Op::Cue, n, 0, 0, 0, 0, 0);
   LOG(i++, Op::Restore, b, 2, 1, 0, 5, 1);   // D-25: restore user mana 5
   LOG(i++, Op::Restore, b, 3, 2, kResourceEverySlot | kResourceFull, 0, 1);   // every slot, to the maximum
   LOG(i++, Op::Raise, b, 2, 2, 0, 2, 1);
   LOG(i++, Op::SelfHit, b, 0, 0, 0, 30, 1);
   LOG(i++, Op::DamageHp, b, 3, 0, kDamageTyped | kDamageQuiet | kDamageNoFaintCheck, 12, 1);
   LOG(i++, Op::Switch, b, 3, static_cast<int>(SwitchMode::Random), 0, kHostPicks, 1);
   LOG(i++, Op::Stage, b, 3, 1 | kStageSilent, 0, 1, 1);   // E13b: stage def +1 silent
   LOG(i++, Op::ResetStages, b, 3, 0, 0, 0, 0);
   LOG(i++, Op::Delay, b, 3, 0, 0, 40, 2);
   CHECK_EQ(g_host.log[i - 1].action.args[1], 3);
   LOG(i++, Op::DelayHit, b, 3, 0, 0, 3, 1);   // G1: delay_hit target 3 foresight
   LOG(i++, Op::CallMove, b, 2, 0, 0, 1, 1);   // G1: use_move user Multi unless touch
   LOG(i++, Op::Reward, n, 0, 0, 0, 25, 1);
   LOG(i++, Op::EndBattle, n, 0, 0, 0, 0, 0);
   CHECK_EQ(g_host.logged, i);
   CHECK_EQ(g_host.nested_runs, 1);
   CHECK_EQ(g_machine.depth, 0);
   CHECK_EQ(g_machine.faults, 0);

   // Verdicts and stops.
   reset(vm_effects::kBank);
   Status s = Status::Done;
   run_hook("status Stopper", Hook::BeforeMove, 0, &s);
   CHECK(s == Status::Stopped);
   run_hook("passive Wall", Hook::TryHit, 0, &s);
   CHECK(s == Status::Immune);
   LOG(0, Op::Immune, n, 0, 0, 0, 64, 1);
   run_hook("volatile Shield", Hook::BeforeAccuracy, 0, &s);
   CHECK(s == Status::Blocked);
   LOG(1, Op::Block, n, 0, 5, 0, 0, 0);
   begin(r, find(vm_effects::kBank, vm_effects::kOwnerNames, "move Failer", Hook::Use), context(0), true);
   CHECK(resume(g_machine, vm_effects::kBank, r, g_host) == Status::Failed);
   begin(r, find(vm_effects::kBank, vm_effects::kOwnerNames, "move Aimer", Hook::Target), context(0), true);
   CHECK(resume(g_machine, vm_effects::kBank, r, g_host) == Status::Done);
   LOG(2, Op::SetTarget, n, 0, 0, 0, 1, 1);
}

/// Yield / resume, Ask / answer, and copying a suspended run.
void test_suspend()
{
   reset(vm_effects::kBank);
   EffectRun r{};
   begin(r, find(vm_effects::kBank, vm_effects::kOwnerNames, "move Multi", Hook::Hit), context(0), true);
   for (int hit = 0; hit < 3; ++hit)
   {
      CHECK(resume(g_machine, vm_effects::kBank, r, g_host) == Status::Yielded);
      CHECK_EQ(r.ctx.dealt, (hit + 1) * 10);
   }
   EffectRun copy = r;   // plain data: a snapshot resumes the same way
   CHECK(resume(g_machine, vm_effects::kBank, r, g_host) == Status::Done);
   CHECK(resume(g_machine, vm_effects::kBank, copy, g_host) == Status::Done);
   CHECK_EQ(g_host.logged, 5);   // three hits, then "done" twice
   CHECK(g_host.log[3].action.op == Op::Text && g_host.log[4].action.op == Op::Text);
   CHECK(resume(g_machine, vm_effects::kBank, r, g_host) == Status::Done);   // finished: nothing more

   reset(vm_effects::kBank);
   begin(r, find(vm_effects::kBank, vm_effects::kOwnerNames, "move Pause", Hook::Use), context(0), true);
   CHECK(resume(g_machine, vm_effects::kBank, r, g_host) == Status::Yielded);
   CHECK_EQ(g_host.logged, 1);
   CHECK(resume(g_machine, vm_effects::kBank, r, g_host) == Status::Done);
   CHECK_EQ(g_host.logged, 2);

   reset(vm_effects::kBank);
   g_host.ask_suspends = true;
   begin(r, find(vm_effects::kBank, vm_effects::kOwnerNames, "move Asker", Hook::Use), context(0), true);
   CHECK(resume(g_machine, vm_effects::kBank, r, g_host) == Status::Asked);
   CHECK(resume(g_machine, vm_effects::kBank, r, g_host) == Status::Asked);   // still waiting
   LOG(0, Op::Ask, PlaceKind::Battler, 2, static_cast<int>(AskKind::LearnMove), 0, 0, 0);
   CHECK(answer(g_machine, vm_effects::kBank, r, g_host, 5) == Status::Done);
   LOG(1, Op::Credit, PlaceKind::None, 0, 0, 0, 5, 1);

   reset(vm_effects::kBank);
   g_host.ask_answer = 9;   // an AI answers at once
   begin(r, find(vm_effects::kBank, vm_effects::kOwnerNames, "move Asker", Hook::Use), context(0), true);
   CHECK(resume(g_machine, vm_effects::kBank, r, g_host) == Status::Done);
   LOG(1, Op::Credit, PlaceKind::None, 0, 0, 0, 9, 1);

   reset(vm_effects::kBank);
   g_host.ask_suspends = true;
   begin(r, find(vm_effects::kBank, vm_effects::kOwnerNames, "move Passer", Hook::Use), context(0), true);
   CHECK(resume(g_machine, vm_effects::kBank, r, g_host) == Status::Asked);
   CHECK(answer(g_machine, vm_effects::kBank, r, g_host, 4) == Status::Done);
   LOG(1, Op::Switch, PlaceKind::Battler, 2, static_cast<int>(SwitchMode::Pass), 0, 4, 1);
   begin(r, find(vm_effects::kBank, vm_effects::kOwnerNames, "move Pusher", Hook::Use), context(0), true);
   CHECK(resume(g_machine, vm_effects::kBank, r, g_host) == Status::Asked);
   CHECK(answer(g_machine, vm_effects::kBank, r, g_host, 3) == Status::Done);
   LOG(3, Op::Switch, PlaceKind::Battler, 2, static_cast<int>(SwitchMode::Ask), 0, 3, 1);
   CHECK_EQ(g_machine.faults, 0);
   CHECK(answer(g_machine, vm_effects::kBank, r, g_host, 3) == Status::Fault);   // not asking
   CHECK(g_machine.last_fault == Fault::Suspend);
}

/// A one-program bank from raw bytes.
struct RawBank
{
   uint8_t code[256];          ///< Code.
   ProgramInfo programs[1];    ///< The program.
   Bank bank;                  ///< The bank.
};

/// Builds a raw bank.
/// @param rb     filled in
/// @param bytes  code
/// @param n      length
void raw(RawBank& rb, const uint8_t* bytes, int n)
{
   std::memcpy(rb.code, bytes, static_cast<size_t>(n));
   rb.programs[0] = ProgramInfo{0, static_cast<uint16_t>(n), 0, 0, 0, 0, 0};
   rb.bank = Bank{rb.code, rb.programs, nullptr, nullptr, static_cast<uint32_t>(n), 1, 0, 0, 0};
}

/// Runs raw code as a hook; returns the status and checks the fault.
/// @param bytes  code
/// @param n      length
/// @param fault  the fault expected (Fault::None: none)
/// @param line   source line
/// @return ctx.value out
int32_t run_raw(const uint8_t* bytes, int n, Fault fault, int line)
{
   static RawBank rb;
   raw(rb, bytes, n);
   reset(rb.bank);
   Context ctx = context(0);
   const Status s = invoke(g_machine, rb.bank, g_host, 0, ctx);
   ++g_checks;
   const bool ok = fault == Fault::None ? s != Status::Fault && g_machine.faults == 0
                                        : s == Status::Fault && g_machine.last_fault == fault && g_machine.faults == 1;
   if (!ok)
   {
      std::printf("FAIL line %d: status %d fault %d (%d faults)\n", line, static_cast<int>(s),
                  static_cast<int>(g_machine.last_fault), g_machine.faults);
      ++g_failures;
   }
   CHECK_EQ(g_machine.depth, 0);
   return ctx.value;
}

#define RAW(fault, ...)                                                                                                \
   [&]                                                                                                                 \
   {                                                                                                                   \
      const uint8_t code_[] = {__VA_ARGS__};                                                                           \
      return run_raw(code_, sizeof code_, fault, __LINE__);                                                            \
   }()

/// Opcode number.
/// @param o  the opcode
/// @return its byte
constexpr uint8_t op(Op o)
{
   return static_cast<uint8_t>(o);
}

/// Hand-assembled programs: the opcodes the compiler does not emit, and every fault.
void test_raw()
{
   // Dup, Swap: 3 5 swap -> 5 3; sub -> 2; dup add -> 4; modify set.
   CHECK_EQ(RAW(Fault::None, op(Op::Push8), 3, op(Op::Push8), 5, op(Op::Swap), op(Op::Sub), op(Op::Dup), op(Op::Add),
                op(Op::Modify), 2, op(Op::End)),
            4);
   // Push16 / Push32 sign extension.
   CHECK_EQ(RAW(Fault::None, op(Op::Push16), 0x00, 0x80, op(Op::Modify), 2, op(Op::End)), -32768);
   CHECK_EQ(RAW(Fault::None, op(Op::Push32), 0xFF, 0xFF, 0xFF, 0xFF, op(Op::Modify), 2, op(Op::End)), -1);
   // Get Battler (the VM answers it) and Subj.
   CHECK_EQ(RAW(Fault::None, op(Op::Get), static_cast<uint8_t>(Ref::Battler), kSubjTarget, 0, op(Op::Subj), kSubjUser,
                op(Op::Add), op(Op::Modify), 2, op(Op::End)),
            5);

   uint8_t overflow[(kEffectStack + 1) * 2 + 1];
   for (int i = 0; i <= kEffectStack; ++i)
   {
      overflow[i * 2] = op(Op::Push8);
      overflow[i * 2 + 1] = 1;
   }
   overflow[(kEffectStack + 1) * 2] = op(Op::End);
   run_raw(overflow, sizeof overflow, Fault::StackOverflow, __LINE__);
   RAW(Fault::StackUnderflow, op(Op::Drop), op(Op::End));
   RAW(Fault::StackUnderflow, op(Op::Push8), 1, op(Op::Add), op(Op::End));
   RAW(Fault::BadLocal, op(Op::Load), kEffectLocals, op(Op::End));
   RAW(Fault::BadLocal, op(Op::Push8), 1, op(Op::Store), 200, op(Op::End));
   RAW(Fault::BadOpcode, 0xEE, op(Op::End));
   RAW(Fault::BadJump, op(Op::Push8), 0, op(Op::Jmp), 0, 0, op(Op::End));   // backwards
   RAW(Fault::BadJump, op(Op::Jmp), 40, 0, op(Op::End));                      // past the end
   RAW(Fault::BadJump, op(Op::Loop), 0, 5, 0, op(Op::End), op(Op::End));      // a back-edge forwards
   CHECK_EQ(RAW(Fault::None, op(Op::Push8), 3, op(Op::Store), 0, op(Op::Loop), 0, 4, 0, op(Op::End)), 0);   // empty body
   RAW(Fault::BadJump, op(Op::Next), 0, 9, 0, op(Op::End));
   RAW(Fault::CallDepth, op(Op::Call), 0, 0, op(Op::End));                    // calls itself
   RAW(Fault::BadProgram, op(Op::Call), 99, 0, op(Op::End));
   RAW(Fault::BadProgram, op(Op::Push8), 1);                                  // runs off the end
   RAW(Fault::BadProgram, op(Op::Push16), 1);                                 // immediate past the end
   // 100000 rounds of a loop: the kEffectOpsPerRun backstop.
   RAW(Fault::OpBudget, op(Op::Push32), 0xA0, 0x86, 0x01, 0x00, op(Op::Store), 0, op(Op::Loop), 0, 7, 0, op(Op::End));
   RAW(Fault::Suspend, op(Op::Yield), op(Op::End));                           // a hook may not suspend
   RAW(Fault::Suspend, op(Op::Ask), 0, kSubjUser, op(Op::End));
   RAW(Fault::BadSubject, op(Op::Get), 0, 0x40, 0, op(Op::End));
   RAW(Fault::BadSubject, op(Op::Push8), 99, op(Op::Store), 0, op(Op::Get), 0, kSubjLocal, 0, op(Op::End));
   RAW(Fault::BadSubject, op(Op::Get), 0, kSubjMove | kSubjSide, 0, op(Op::End));
   RAW(Fault::BadSubject, op(Op::Push8), 1, op(Op::Heal), 0x7E, op(Op::End));
   RAW(Fault::BadOperand, op(Op::Get), 200, 0, 0, op(Op::End));
   RAW(Fault::BadOperand, op(Op::Has), 200, 0, 0, op(Op::End));
   RAW(Fault::BadOperand, op(Op::Push8), 1, op(Op::Set), static_cast<uint8_t>(Ref::Value), 0, 0, op(Op::End));
   RAW(Fault::BadOperand, op(Op::Push8), 1, op(Op::Modify), 9, op(Op::End));
   RAW(Fault::BadOperand, op(Op::Push8), 1, op(Op::Pick), 0, op(Op::End));
   RAW(Fault::BadOperand, op(Op::ForEach), 9, 0, 0, 7, 0, op(Op::Next), 0, 6, 0, op(Op::End));
}

/// The depth limit: a primitive that starts its own program again.
void test_depth()
{
   static RawBank rb;
   const uint8_t code[] = {op(Op::Inflict), kSubjHolder, 0, 0, op(Op::Drop), op(Op::End)};
   raw(rb, code, sizeof code);
   reset(rb.bank);
   g_host.recurse = true;
   g_host.recurse_program = 0;
   Context ctx = context(0);
   CHECK(invoke(g_machine, rb.bank, g_host, 0, ctx) == Status::Done);
   CHECK_EQ(g_host.nested_runs, kMaxEffectDepth);   // runs at depths 1-4; the fifth is skipped
   CHECK_EQ(g_machine.faults, 1);
   CHECK(g_machine.last_fault == Fault::Depth);
   CHECK_EQ(g_machine.depth, 0);
}

/// The generated point handlers.
void test_points()
{
   PointCtx c{true, 0};
   // passive Wall point power (fn 1): when move.has(touch) { modify by 1.5; modify add holder.level; }
   CHECK_EQ(vm_effects::point_handler(1, c, 100), 150 + static_cast<int32_t>(Ref::Level) * 10 + kSubjHolder);
   c.has = false;
   CHECK_EQ(vm_effects::point_handler(1, c, 100), 100);
   // move Aimer point accuracy (fn 0): user == holder reads Ref::Battler of each (they differ here)
   CHECK_EQ(vm_effects::point_handler(0, c, 100), 101);
   c.base = -static_cast<int32_t>(Ref::Battler) * 10;   // point_get(Battler, s) = s: still different subjects
   CHECK_EQ(vm_effects::point_handler(0, c, 100), 101);
   CHECK_EQ(vm_effects::point_handler(99, c, 100), 100);   // unknown fn: unchanged
}

} // namespace

int main()
{
   test_values();
   test_reads_writes();
   test_rng();
   test_odds();
   test_primitives();
   test_suspend();
   test_raw();
   test_depth();
   test_points();
   if (g_failures != 0)
   {
      std::printf("%d failure(s)\n", g_failures);
      return 1;
   }
   std::printf("effect VM tests passed (%d checks)\n", g_checks);
   return 0;
}
